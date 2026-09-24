import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { test } from "node:test";
import { authorizeLive, liveSession, relayLive } from "../lib/live.ts";

const token = "a".repeat(64);
const request = (headers = {}, url = "https://device.example/api/live") =>
  new Request(url, {
    headers: { Authorization: `Bearer ${token}`, Upgrade: "websocket", ...headers },
  });
const options = { apiKey: "server-key", deviceToken: token, checkLimit: async () => ({ rateLimited: false }) };

class Socket extends EventEmitter {
  readyState = 1;
  bufferedAmount = 0;
  sent = [];
  send(data, options) {
    this.sent.push({ data, options });
  }
  close() {
    this.terminate();
  }
  terminate() {
    if (this.readyState === 3) return;
    this.readyState = 3;
    queueMicrotask(() => this.emit("close"));
  }
  json(value) {
    this.emit("message", Buffer.from(JSON.stringify(value)), false);
  }
  binary(value) {
    this.emit("message", Buffer.from(value), true);
  }
  controls() {
    return this.sent.filter((v) => typeof v.data === "string").map((v) => JSON.parse(v.data));
  }
}
function fixture() {
  const device = new Socket(),
    upstream = new Socket();
  const done = relayLive(device, "server-key", "device-hash", () => upstream);
  upstream.emit("open");
  const started = () => upstream.json({ type: "session.started", session: liveSession });
  const end = async () => {
    upstream.json({ type: "session.closed", usage: { seconds: 1.25 } });
    await done;
  };
  return { device, upstream, done, started, end };
}

test("Live authentication fails closed and never consumes quota for bad credentials", async () => {
  let checks = 0;
  const config = {
    ...options,
    checkLimit: async () => {
      ++checks;
      return { rateLimited: false };
    },
  };
  assert.equal((await authorizeLive(request({ Authorization: "Bearer invalid" }), config)).status, 401);
  assert.equal((await authorizeLive(request({ Origin: "https://other.example" }), config)).status, 403);
  assert.equal((await authorizeLive(request({}, "https://device.example/api/live?model=other"), config)).status, 400);
  assert.equal((await authorizeLive(request({ Upgrade: "http" }), config)).status, 400);
  assert.equal((await authorizeLive(request({ Authorization: `Bearer ${"b".repeat(64)}` }), config)).status, 401);
  assert.equal((await authorizeLive(request(), { ...config, apiKey: undefined })).status, 503);
  assert.equal(checks, 0);
  assert.equal(typeof (await authorizeLive(request(), config)), "string");
  assert.equal(checks, 1);
  assert.equal((await authorizeLive(request(), { ...config, deviceToken: `${token}\n` })).status, 503);
  assert.equal(
    (await authorizeLive(request(), { ...config, checkLimit: async () => ({ rateLimited: true }) })).status,
    429,
  );
  assert.equal(
    (await authorizeLive(request(), { ...config, checkLimit: async () => ({ error: "missing" }) })).status,
    503,
  );
  assert.equal(
    (
      await authorizeLive(request(), {
        ...config,
        checkLimit: async () => {
          throw new Error("internal failure");
        },
      })
    ).status,
    503,
  );
});

test("fixed session, binary PCM bridge, private events and graceful final usage", async () => {
  const f = fixture();
  assert.deepEqual(f.upstream.controls()[0], { type: "session.start", session: liveSession });
  f.started();
  f.device.binary([1, 0, 2, 0]);
  assert.deepEqual(f.upstream.controls()[1], { type: "session.input_audio.append", audio: "AQACAA==" });
  f.upstream.json({ type: "session.output_audio.delta", delta: Buffer.alloc(1600, 1).toString("base64") });
  const audio = f.device.sent.filter((v) => v.options?.binary).map((v) => v.data);
  assert.deepEqual(
    audio.map((v) => v.length),
    [1024, 576],
  );
  assert.deepEqual(Buffer.concat(audio), Buffer.alloc(1600, 1));
  const before = f.device.sent.length;
  f.upstream.json({ type: "session.input_transcript.delta", delta: "private conversation" });
  assert.equal(f.device.sent.length, before);
  f.device.json({ type: "close" });
  assert.equal(f.upstream.controls().at(-1).type, "session.close");
  const sent = f.upstream.sent.length;
  f.device.binary([0, 0]);
  assert.equal(f.upstream.sent.length, sent);
  await f.end();
  assert.deepEqual(f.device.controls().at(-1), { type: "closed", finalized: true, seconds: 1.25 });
});

test("device close while connecting is finalized as soon as session starts", async () => {
  const f = fixture();
  f.device.terminate();
  await new Promise((resolve) => queueMicrotask(resolve));
  f.started();
  assert.equal(f.upstream.controls().at(-1).type, "session.close");
  await f.end();
});

test("backpressure and malformed audio stop the session instead of growing queues", async () => {
  for (const scenario of ["output", "input", "odd", "override", "early"]) {
    const f = fixture();
    if (scenario !== "early") f.started();
    if (scenario === "output") {
      f.device.bufferedAmount = 16000;
      f.upstream.json({ type: "session.output_audio.delta", delta: "AAA=" });
    } else if (scenario === "input") {
      f.upstream.bufferedAmount = 16001;
      f.device.binary([0, 0]);
    } else if (scenario === "override") {
      f.device.json({ type: "session.start", session: { model: "other" } });
    } else f.device.binary(scenario === "odd" ? [0] : [0, 0]);
    assert.equal(f.device.controls().at(-1).type, "stopping");
    if (scenario === "early") f.started();
    assert.equal(f.upstream.controls().at(-1).type, "session.close");
    await f.end();
  }
});

test("error bodies are not forwarded and transport loss never reports final usage", async () => {
  const f = fixture();
  f.started();
  f.upstream.json({ type: "error", error: { message: "private error body" } });
  assert.equal(f.device.controls().at(-1).reason, "live_api");
  f.upstream.terminate();
  await f.done;
  assert.equal(JSON.stringify(f.device.sent).includes("private error body"), false);
  assert.deepEqual(f.device.controls().at(-1), { type: "closed", finalized: false });
});

test("idle timeout closes upstream, then bounds failed finalization", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout", "setInterval", "Date"] });
  const f = fixture();
  f.started();
  t.mock.timers.tick(6000);
  assert.equal(f.upstream.controls().at(-1).type, "session.close");
  t.mock.timers.tick(15000);
  await f.done;
  assert.deepEqual(f.device.controls().at(-1), { type: "closed", finalized: false });
});

test("rate budget cannot accumulate during silence and fixed session deadline closes", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout", "setInterval", "Date"] });
  const f = fixture();
  f.started();
  for (let i = 0; i < 240; ++i) {
    // Keep transport alive; quiet time must not permit a large PCM burst later.
    f.device.binary([0, 0]);
    t.mock.timers.tick(1000);
  }
  assert.equal(f.device.controls().at(-1).reason, "time_limit");
  await f.end();
});

test("microphone floods are rejected even after a long quiet connection", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout", "setInterval", "Date"] });
  const f = fixture();
  f.started();
  for (let i = 0; i < 10; ++i) {
    f.device.binary([0, 0]);
    t.mock.timers.tick(1000);
  }
  for (let i = 0; i < 16; ++i) f.device.binary(Buffer.alloc(2048));
  assert.equal(f.device.controls().at(-1).reason, "input_backpressure");
  await f.end();
});

test("backend usage exposes only validated counts", async () => {
  const f = fixture();
  f.started();
  f.upstream.json({
    type: "response.event",
    event: {
      type: "response.completed",
      response: {
        output: "private answer",
        usage: { input_tokens: 22, output_tokens: 7, input_tokens_details: { cached_tokens: 10 } },
      },
    },
  });
  assert.deepEqual(f.device.controls().at(-1), {
    type: "backend_usage",
    input_tokens: 22,
    output_tokens: 7,
    cached_tokens: 10,
  });
  assert.equal(JSON.stringify(f.device.sent).includes("private answer"), false);
  await f.end();
});
