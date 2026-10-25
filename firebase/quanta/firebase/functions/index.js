"use strict";

const crypto = require("node:crypto");
const {initializeApp} = require("firebase-admin/app");
const {getDatabase} = require("firebase-admin/database");
const {onCall, HttpsError} = require("firebase-functions/v2/https");
const {setGlobalOptions} = require("firebase-functions/v2");

const PROJECT = "quanta-f9c4c";
initializeApp({databaseURL: "https://quanta-f9c4c-default-rtdb.firebaseio.com"});
setGlobalOptions({maxInstances: 10});
const db = getDatabase();
const clean = (value, max) => String(value ?? "").trim().slice(0, max);

exports.claimQuantaDevice = onCall(
  {region: "us-central1", enforceAppCheck: false},
  async (request) => {
    const actual = process.env.GCLOUD_PROJECT || process.env.GCP_PROJECT;
    if (actual && actual !== PROJECT) {
      throw new HttpsError("failed-precondition", "Wrong Firebase project");
    }

    const uid = request.auth?.uid;
    if (!uid) throw new HttpsError("unauthenticated", "Sign in first");

    const deviceId = clean(request.data?.deviceId, 60).toLowerCase();
    const code = clean(request.data?.claimCode, 64).toUpperCase();
    const alias = clean(request.data?.alias, 80);
    const customerName = clean(request.data?.customerName, 80);

    if (!/^quantacontroller[1-9]\d*$/.test(deviceId) ||
        !/^[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(code)) {
      throw new HttpsError("invalid-argument", "Invalid controller ID or claim code");
    }

    const hash = crypto.createHash("sha256").update(code).digest("hex");
    const ref = db.ref(`deviceClaims/${deviceId}`);

    // Load the actual server value before the transaction. Firebase transactions
    // can first call the update callback with null even for an existing record.
    const snapshot = await ref.get();
    const initial = snapshot.val();
    if (!initial || initial.codeHash !== hash) {
      throw new HttpsError("permission-denied", "Invalid or already used claim code");
    }
    if (initial.status !== "unclaimed" &&
        !(initial.status === "claimed" && initial.ownerUid === uid)) {
      throw new HttpsError("permission-denied", "Invalid or already used claim code");
    }

    const claimedAt = Date.now();
    let firstCall = true;
    const result = await ref.transaction((record) => {
      // The first null is a local-cache placeholder. Returning the known record
      // triggers a server-side compare-and-swap; the server retries with its
      // current value if it changed. Never trust this cached value on a retry.
      if (firstCall && record === null) {
        firstCall = false;
        return {
          ...initial,
          status: "claimed",
          ownerUid: uid,
          claimedAt,
        };
      }
      firstCall = false;
      if (!record || record.codeHash !== hash) return;
      if (record.status === "claimed" && record.ownerUid === uid) return record;
      if (record.status !== "unclaimed") return;
      return {...record, status: "claimed", ownerUid: uid, claimedAt};
    }, undefined, false);

    if (!result.committed) {
      throw new HttpsError("permission-denied", "Invalid or already used claim code");
    }
    const record = result.snapshot.val();
    if (!record || record.ownerUid !== uid || record.codeHash !== hash) {
      throw new HttpsError("permission-denied", "Controller already claimed");
    }

    const updates = {};
    updates[`users/${uid}/devices/${deviceId}`] = {
      alias: alias || deviceId,
      claimedAt: record.claimedAt,
    };
    updates[`users/${uid}/profile`] = {
      name: customerName || request.auth.token.name || "",
      email: request.auth.token.email || "",
    };
    updates[`devices/${deviceId}/ownerUID`] = uid;
    updates[`devices/${deviceId}/claimed`] = true;
    updates[`devices/${deviceId}/claimedAt`] = record.claimedAt;
    if (alias) updates[`devices/${deviceId}/alias`] = alias;
    updates[`claimInventory/${hash}/status`] = "CLAIMED";
    updates[`claimInventory/${hash}/ownerUid`] = uid;
    updates[`claimInventory/${hash}/claimedAt`] = record.claimedAt;
    await db.ref().update(updates);

    return {success: true, deviceId};
  },
);

// TEST ONLY: "Reset & Start Over" for testers. Does nothing unless QUANTA_TEST_BUILD=true in functions/.env.
exports.testResetQuantaDevice = require("./testResetQuanta").testResetQuantaDevice;
