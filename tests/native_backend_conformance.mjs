// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
// Offline integration test. Arguments: compiled product-test executable, backend checkout.
import assert from "node:assert/strict";
import {execFileSync} from "node:child_process";
import {pathToFileURL} from "node:url";
import {join, isAbsolute} from "node:path";
import {DatabaseSync} from "node:sqlite";
const [binary, backend] = process.argv.slice(2);
assert.equal(process.argv.length, 4);
assert.ok(isAbsolute(binary) && isAbsolute(backend));
const load = relative => import(pathToFileURL(join(backend, relative)));
const {parseBrakeMessage, canonicalize} = await load("out/backend/brake-data-contract.js");
const {BrakeDataStore} = await load("out/backend/brake-data-store.js");
const {applyMigrations, loadMigrations} = await load("out/backend/migrations.js");
const bytes = execFileSync(binary, ["--emit-native-conformance"], {encoding:"utf8", timeout:30000, maxBuffer:1024*1024}).trim().split("\n");
const db = new DatabaseSync(":memory:");
const now = "2026-09-11T12:00:00.000Z";
try {
  applyMigrations(db, loadMigrations(join(backend,"migrations")), now);
  const store = new BrakeDataStore(db), kinds = new Set();
  for (const line of bytes) {
    const value = JSON.parse(line), parsed = parseBrakeMessage(line);
    assert.equal(line, canonicalize(value)); kinds.add(value.messageType);
    assert.equal(value.schemaVersion, 2); assert.equal(value.serviceVersion, "18.0.0");
    assert.equal(Object.hasOwn(value,"serviceArtifactSha256"), false);
    assert.equal(Object.hasOwn(value,"modelArtifactSha256"), false);
    const first = store.ingest(parsed, now), repeated = store.ingest(parsed, now);
    assert.equal(first.httpStatus, 201); assert.equal(repeated.httpStatus, 200);
    assert.equal(first.acknowledgement.receiptId, repeated.acknowledgement.receiptId);
    assert.equal(first.acknowledgement.schemaVersion, 1);
    const retained = db.prepare("SELECT canonical_message FROM messages WHERE message_type=? AND canonical_message=?").get(value.messageType,line);
    assert.equal(retained.canonical_message,line);
  }
  assert.equal(kinds.size,5);
  const summary=store.query("WINDOW",JSON.parse(bytes[0]).unitSystemUid,10,null).items[0];
  assert.equal(summary.messageSchemaVersion,2);
  assert.equal(db.prepare("SELECT count(*) AS count FROM quarantine").get().count,0);
  console.log("PASS five native Brake message kinds: real C++ serialization, backend validation/storage, exact retry receipts and window correlation");
} finally {db.close();}
