"use strict";

const crypto = require("node:crypto");
const {initializeApp} = require("firebase-admin/app");
const {getDatabase} = require("firebase-admin/database");
const {onCall, HttpsError} = require("firebase-functions/v2/https");
const {setGlobalOptions} = require("firebase-functions/v2");

const PROJECT_ID = "quanta-f9c4c";
const DATABASE_URL = "https://quanta-f9c4c-default-rtdb.firebaseio.com";

initializeApp({databaseURL: DATABASE_URL});
setGlobalOptions({maxInstances: 10});

// Manufacturing must first provision /deviceClaims/<deviceId> with
// {codeHash: SHA256(uppercase claim code), status: "unclaimed"}.
// Claim codes must never be stored as plaintext in the database.
exports.claimQuantaDevice = onCall({
  region: "us-central1",
  enforceAppCheck: false, // Enable once the customer app is configured for App Check.
}, async (request) => {
  if (process.env.GCLOUD_PROJECT && process.env.GCLOUD_PROJECT !== PROJECT_ID) {
    throw new HttpsError("failed-precondition", "Wrong Firebase project");
  }
  const uid = request.auth?.uid;
  if (!uid) throw new HttpsError("unauthenticated", "Sign in first");

  const deviceId = String(request.data?.deviceId ?? "").trim().toLowerCase();
  const claimCode = String(request.data?.claimCode ?? "").trim().toUpperCase();
  if (!/^quantacontroller[1-9]\d*$/.test(deviceId) ||
      !/^[A-Z0-9-]{8,64}$/.test(claimCode)) {
    throw new HttpsError("invalid-argument", "Invalid controller ID or claim code");
  }

  const submittedHash = crypto.createHash("sha256").update(claimCode).digest("hex");
  const claimRef = getDatabase().ref(`deviceClaims/${deviceId}`);
  const result = await claimRef.transaction((record) => {
    if (!record || record.status !== "unclaimed" ||
        typeof record.codeHash !== "string" ||
        record.codeHash.length !== submittedHash.length) return;
    const expected = Buffer.from(record.codeHash, "hex");
    const received = Buffer.from(submittedHash, "hex");
    if (expected.length !== 32 || !crypto.timingSafeEqual(expected, received)) return;
    return {...record, status: "claimed", ownerUid: uid,
      claimedAt: Date.now()};
  }, undefined, false);

  if (!result.committed) {
    throw new HttpsError("permission-denied", "Invalid or already used claim code");
  }
  return {success: true, deviceId};
});
