// =============================================================================
// QUANTA TEST-ONLY Cloud Function: testResetQuantaDevice
//
// Puts a controller back to "brand new, claim code not used yet" so a tester can
// repeat sign-up -> claim -> Wi-Fi/online check -> setup assistant.
//
// DEVICE FIRST: before touching anything in the cloud this writes
//   devices/{id}/commands/testReset = { trigger:true, nonce, ts, requestedBy }
// and waits (up to 25 s) for the controller to wipe its own setup and write back
//   devices/{id}/commands/testReset/ack = <same nonce string>
// No ack -> nothing is changed and the tester sees "controller did not confirm".
// (Set QUANTA_SKIP_DEVICE_RESET=true in functions/.env to skip this step.)
// Then the cloud half:
//   /deviceClaims/{id}      status -> "unclaimed", ownerUid + claimedAt removed
//   /claimInventory/{hash}  status -> "UNCLAIMED", ownerUid + claimedAt removed
//   /devices/{id}           ownerUID, claimed, claimedAt, alias removed
//   /users/{uid}/devices/{id} removed
//   the caller's login + /users/{uid} deleted, unless it is the admin login or
//   it owns other controllers (then only this controller is un-linked)
//
// Kept untouched: the claim code hash, everything the manufacture step wrote,
// and anything the controller itself reports under /devices/{id}/state.
//
// Server-side kill switch (functions/.env):
//   QUANTA_TEST_BUILD=true
//   QUANTA_TEST_DEVICE_IDS=quantacontroller1        (comma separated)
// Production: leave QUANTA_TEST_BUILD unset/false, or do not export this function.
//
// In functions/index.js add:
//   exports.testResetQuantaDevice = require("./testResetQuanta").testResetQuantaDevice;
// =============================================================================
"use strict";

const {onCall, HttpsError} = require("firebase-functions/v2/https");
const {getDatabase} = require("firebase-admin/database");
const {getAuth} = require("firebase-admin/auth");

const TEST_BUILD = process.env.QUANTA_TEST_BUILD === "true";
const SKIP_DEVICE_RESET = process.env.QUANTA_SKIP_DEVICE_RESET === "true";
const DEVICE_ACK_TIMEOUT_MS = 25000;

// Resolves true when `ref` becomes equal to `expected`, false on timeout.
function waitForAck(ref, expected, timeoutMs) {
  return new Promise((resolve) => {
    let finished = false;
    let timer = null;
    const handler = (snap) => { if (snap.val() === expected) finish(true); };
    function finish(result) {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      ref.off("value", handler);
      resolve(result);
    }
    timer = setTimeout(() => finish(false), timeoutMs);
    ref.on("value", handler);
  });
}
const TEST_DEVICE_IDS = String(process.env.QUANTA_TEST_DEVICE_IDS || "")
    .split(",").map((x) => x.trim().toLowerCase()).filter(Boolean);

// Never delete this login.
const PROTECTED_EMAILS = ["markdamonrood@gmail.com"];

exports.testResetQuantaDevice = onCall(
    {region: "us-central1", timeoutSeconds: 60, enforceAppCheck: false},
    async (request) => {
      if (!TEST_BUILD) {
        throw new HttpsError("failed-precondition", "Test reset is disabled on this deployment.");
      }
      const uid = request.auth && request.auth.uid;
      if (!uid) throw new HttpsError("unauthenticated", "Sign in first.");

      const deviceId = String((request.data && request.data.deviceId) || "").trim().toLowerCase();
      if (!/^quantacontroller[1-9]\d*$/.test(deviceId)) {
        throw new HttpsError("invalid-argument", "Device ID must look like quantacontroller1.");
      }
      if (!TEST_DEVICE_IDS.includes(deviceId)) {
        throw new HttpsError("permission-denied", `${deviceId} is not on the test-device list.`);
      }

      const db = getDatabase();
      const claimRef = db.ref(`deviceClaims/${deviceId}`);
      const claim = (await claimRef.get()).val();
      if (!claim) throw new HttpsError("not-found", "No claim record for that controller.");
      if (claim.ownerUid && claim.ownerUid !== uid) {
        throw new HttpsError("permission-denied", "This controller is claimed by a different account.");
      }
      if (!claim.codeHash) {
        throw new HttpsError("failed-precondition", "Claim record has no codeHash; cannot restore.");
      }
      const hash = String(claim.codeHash);

      // Step 1: make the controller wipe its own setup FIRST, so an offline controller
      // leaves everything untouched (no half reset).
      if (!SKIP_DEVICE_RESET) {
        const cmdRef = db.ref(`devices/${deviceId}/commands/testReset`);
        const nonce = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;   // text, not a number
        await cmdRef.set({trigger: true, nonce, ts: Date.now(), requestedBy: uid});
        const acked = await waitForAck(cmdRef.child("ack"), nonce, DEVICE_ACK_TIMEOUT_MS);
        await cmdRef.remove();
        if (!acked) {
          throw new HttpsError("deadline-exceeded",
              "The controller did not confirm the reset. Make sure it is powered on and online, then try again. Nothing was changed.");
        }
      }

      // Step 2: un-spend the claim code. update() with nulls removes just the fields the claim added.
      const updates = {};
      updates[`deviceClaims/${deviceId}/status`] = "unclaimed";
      updates[`deviceClaims/${deviceId}/ownerUid`] = null;
      updates[`deviceClaims/${deviceId}/claimedAt`] = null;
      updates[`claimInventory/${hash}/status`] = "UNCLAIMED";
      updates[`claimInventory/${hash}/ownerUid`] = null;
      updates[`claimInventory/${hash}/claimedAt`] = null;
      updates[`devices/${deviceId}/ownerUID`] = null;
      updates[`devices/${deviceId}/claimed`] = null;
      updates[`devices/${deviceId}/claimedAt`] = null;
      updates[`devices/${deviceId}/alias`] = null;
      updates[`users/${uid}/devices/${deviceId}`] = null;
      await db.ref().update(updates);

      // Delete the login last, and only if it is a throwaway test account.
      const mapped = Object.keys((await db.ref(`users/${uid}/devices`).get()).val() || {});
      const email = String((request.auth.token && request.auth.token.email) || "").toLowerCase();
      let accountDeleted = false;
      if (PROTECTED_EMAILS.includes(email)) {
        // keep the admin login
      } else if (mapped.length > 0) {
        // login owns other controllers: keep it
      } else {
        await db.ref(`users/${uid}`).remove();
        await getAuth().deleteUser(uid);
        accountDeleted = true;
      }
      return {ok: true, deviceId, accountDeleted};
    },
);
