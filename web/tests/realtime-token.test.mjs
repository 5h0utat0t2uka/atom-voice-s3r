import assert from "node:assert/strict";
import { afterEach, mock, test } from "node:test";
import { issueRealtimeToken, realtimeModel } from "../lib/realtime-token.ts";

const deviceToken = "a".repeat(64);
const options = () => ({ apiKey: "test-api-secret", deviceToken, checkLimit: async () => ({ rateLimited: false }) });
const request = (headers = {}, body) =>
  new Request("https://device.example/api/realtime/token", {
    method: "POST",
    headers: { Authorization: `Bearer ${deviceToken}`, ...headers },
    body,
  });
afterEach(() => mock.restoreAll());

test("unauthenticated requests cannot issue tokens or consume authenticated quota", async () => {
  const fetch = mock.method(globalThis, "fetch");
  const config = options();
  config.checkLimit = mock.fn(config.checkLimit);
  for (const authorization of ["Bearer wrong", `Bearer ${"b".repeat(64)}`]) {
    const response = await issueRealtimeToken(request({ Authorization: authorization }), config);
    assert.equal(response.status, 401);
  }
  assert.equal(config.checkLimit.mock.callCount(), 0);
  assert.equal(fetch.mock.callCount(), 0);
});

test("missing configuration, browser origin and client session override are rejected", async () => {
  const fetch = mock.method(globalThis, "fetch");
  assert.equal((await issueRealtimeToken(request(), { ...options(), deviceToken: undefined })).status, 503);
  assert.equal((await issueRealtimeToken(request(), { ...options(), deviceToken: `${deviceToken}\n` })).status, 503);
  assert.equal((await issueRealtimeToken(request({ Origin: "https://evil.example" }), options())).status, 403);
  assert.equal((await issueRealtimeToken(request({}, '{"model":"other"}'), options())).status, 400);
  assert.equal(fetch.mock.callCount(), 0);
});

test("an empty streamed POST body is accepted, while GET cannot issue credentials", async () => {
  const fetch = mock.method(globalThis, "fetch", async () =>
    Response.json({ value: "ek_example", expires_at: Math.floor(Date.now() / 1000) + 60 }),
  );
  const empty = new Request("https://device.example/api/realtime/token", {
    method: "POST",
    headers: { Authorization: `Bearer ${deviceToken}` },
    body: new ReadableStream({ start: (controller) => controller.close() }),
    duplex: "half",
  });
  assert.equal((await issueRealtimeToken(empty, options())).status, 200);
  assert.equal((await issueRealtimeToken(new Request(empty.url), options())).status, 405);
  assert.equal(fetch.mock.callCount(), 1);
});

test("quota exceeded, missing firewall rule and firewall outage all prevent issuing", async () => {
  const fetch = mock.method(globalThis, "fetch");
  for (const [result, expected] of [
    [{ rateLimited: true }, 429],
    [{ rateLimited: false, error: "not-found" }, 503],
  ]) {
    const response = await issueRealtimeToken(request(), { ...options(), checkLimit: async () => result });
    assert.equal(response.status, expected);
  }
  const response = await issueRealtimeToken(request(), {
    ...options(),
    checkLimit: async () => {
      throw new Error("private internal failure");
    },
  });
  assert.equal(response.status, 503);
  assert.equal(fetch.mock.callCount(), 0);
});

test("success returns only short-lived credentials and fixed model with no-store", async () => {
  const expiresAt = Math.floor(Date.now() / 1000) + 60;
  const fetch = mock.method(globalThis, "fetch", async (_url, init) => {
    const payload = JSON.parse(init.body);
    assert.deepEqual(payload.expires_after, { anchor: "created_at", seconds: 60 });
    assert.equal(payload.session.audio.input.turn_detection, null);
    assert.equal(payload.session.audio.output.format.rate, 24000);
    assert.equal(payload.session.model, realtimeModel);
    return Response.json({ value: "ek_example", expires_at: expiresAt, extra: "must-not-return" });
  });
  const response = await issueRealtimeToken(request(), options());
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("cache-control"), "no-store");
  assert.deepEqual(await response.json(), { value: "ek_example", expires_at: expiresAt, model: realtimeModel });
  assert.equal(fetch.mock.callCount(), 1);
});

test("upstream errors, oversized bodies and expired secrets fail without leaking response data", async () => {
  for (const upstream of [
    new Response("test-api-secret", { status: 401 }),
    new Response("x".repeat(20000)),
    Response.json({ value: "ek_expired", expires_at: 1 }),
  ]) {
    mock.method(globalThis, "fetch", async () => upstream);
    const response = await issueRealtimeToken(request(), options());
    assert.equal(response.status, 502);
    assert.doesNotMatch(await response.text(), /test-api-secret|ek_expired/);
    mock.restoreAll();
  }
});
