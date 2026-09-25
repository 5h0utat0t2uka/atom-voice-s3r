import WebSocket, { type RawData } from "ws";
import { authenticateDevice, type DeviceOptions, limitDevice, reply } from "./device-auth.ts";

export const liveSession = {
  model: "gpt-live-1",
  store: false,
  instructions: `日本語で明瞭に、自然な速さで簡潔に会話するアシスタントです。
Backchannel policy: 適度な短い相づちを使ってください。
Interruption policy: 相手が割り込んだら説明を止めて聞いてください。
Delegation policy:
Backend tools: 知識に基づく質問への回答、計算、推論、Web検索。外部サービスを変更する操作はできません。
Delegate to the backend when: 知識、計算、慎重な推論、最新情報の確認が必要な質問や訂正、Web検索の依頼を受けたとき。
Do not delegate to the backend when: あいさつ、会話中の結果の繰り返し、質問を理解するための短い確認。
バックエンドの結果が必要な回答は、その結果を待ってから伝えてください。`,
  audio: { format: { type: "audio/pcm", rate: 16000 }, output: { voice: "marin" } },
  delegation: {
    type: "responses",
    responses: {
      model: "gpt-6-luna",
      reasoning: { effort: "low" },
      tools: [{ type: "web_search" }],
      tool_choice: "auto",
      instructions: "日本語で簡潔に、最新情報が必要な場合や検索を依頼された場合はWeb検索を使用して回答してください。",
    },
  },
};
export const liveLimitMs = 240_000;
const queuedBytesLimit = 16_000; // 500 ms of mono PCM16 at 16 kHz.

export async function authorizeLive(request: Request, options: DeviceOptions): Promise<string | Response> {
  const deviceId = authenticateDevice(request, options);
  if (deviceId instanceof Response) return deviceId;
  if (request.method !== "GET" || request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
    return reply({ error: "websocket_required" }, 400);
  }
  if (new URL(request.url).search || request.body !== null) return reply({ error: "invalid_request" }, 400);
  return (await limitDevice(deviceId, options)) ?? deviceId;
}

function bytes(data: RawData): Buffer {
  return Array.isArray(data) ? Buffer.concat(data) : Buffer.from(data as ArrayBuffer);
}

// The device protocol only carries PCM and close requests. API credentials,
// prompts, tools, transcripts and upstream error bodies never reach the device.
export function relayLive(
  device: WebSocket,
  apiKey: string,
  deviceId: string,
  connect = () =>
    new WebSocket("wss://api.openai.com/v1/live/sessions", {
      headers: { Authorization: `Bearer ${apiKey}`, "OpenAI-Safety-Identifier": deviceId },
      handshakeTimeout: 10_000,
      maxPayload: 256 * 1024,
      perMessageDeflate: false,
    }),
): Promise<void> {
  return new Promise((resolve) => {
    const upstream = connect();
    let started = false,
      closing = false,
      finished = false,
      finalized = false;
    let lastInputAt = Date.now(),
      budgetAt = Date.now(),
      inputBudget = queuedBytesLimit;
    let closeTimer: ReturnType<typeof setTimeout> | undefined;
    const startTimer = setTimeout(() => stop("start_timeout"), 15_000);
    const limitTimer = setTimeout(() => stop("time_limit"), liveLimitMs);
    const watchdog = setInterval(() => {
      if (started && !closing && Date.now() - lastInputAt > 5000) stop("input_timeout");
    }, 1000);

    function control(value: object) {
      if (device.readyState === WebSocket.OPEN) device.send(JSON.stringify(value));
    }
    function finish() {
      if (finished) return;
      finished = true;
      clearTimeout(startTimer);
      clearTimeout(limitTimer);
      clearTimeout(closeTimer);
      clearInterval(watchdog);
      // terminate also releases a peer that fails to complete the close handshake.
      upstream.terminate();
      device.terminate();
      resolve();
    }
    function stop(reason: string) {
      if (finished || closing || finalized) return;
      closing = true;
      control({ type: "stopping", reason });
      if (started && upstream.readyState === WebSocket.OPEN) upstream.send('{"type":"session.close"}');
      closeTimer = setTimeout(() => {
        control({ type: "closed", finalized: false });
        finish();
      }, 15_000);
    }
    function sendAudio(audio: Buffer) {
      if (device.readyState !== WebSocket.OPEN || device.bufferedAmount + audio.length > queuedBytesLimit) {
        stop("playback_backpressure");
        return;
      }
      // Small binary messages keep ESP32 frame parsing bounded.
      for (let offset = 0; offset < audio.length; offset += 1024) {
        device.send(audio.subarray(offset, offset + 1024), { binary: true });
      }
    }

    device.on("message", (data, binary) => {
      if (closing || finished) return;
      const chunk = bytes(data);
      if (!binary) {
        if (chunk.toString() === '{"type":"close"}') stop("button");
        else stop("invalid_command");
        return;
      }
      if (!started || !chunk.length || chunk.length > 2048 || chunk.length % 2) {
        stop("invalid_audio");
        return;
      }
      // A bounded token bucket allows Wi-Fi jitter, never a growing catch-up burst.
      const now = Date.now();
      inputBudget = Math.min(queuedBytesLimit, inputBudget + Math.max(0, now - budgetAt) * 32);
      budgetAt = now;
      if (chunk.length > inputBudget || upstream.bufferedAmount > queuedBytesLimit) {
        stop("input_backpressure");
        return;
      }
      inputBudget -= chunk.length;
      lastInputAt = now;
      upstream.send(JSON.stringify({ type: "session.input_audio.append", audio: chunk.toString("base64") }));
    });
    device.on("close", () => {
      if (finalized) finish();
      else stop("device_disconnected");
    });
    device.on("error", () => stop("device_error"));
    upstream.on("open", () => upstream.send(JSON.stringify({ type: "session.start", session: liveSession })));
    upstream.on("message", (data, binary) => {
      if (finished) return;
      try {
        if (binary) throw new Error("Unexpected binary data");
        const event = JSON.parse(bytes(data).toString());
        if (event.type === "session.started") {
          if (
            started ||
            event.session?.model !== liveSession.model ||
            event.session?.audio?.format?.type !== "audio/pcm" ||
            event.session.audio.format.rate !== 16000
          ) {
            stop("session_format");
            return;
          }
          started = true;
          budgetAt = lastInputAt = Date.now();
          clearTimeout(startTimer);
          if (closing) upstream.send('{"type":"session.close"}');
          else control({ type: "ready", rate: 16000, limit_seconds: liveLimitMs / 1000 });
        } else if (event.type === "session.output_audio.delta" && !closing) {
          if (
            !started ||
            typeof event.delta !== "string" ||
            !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(event.delta)
          ) {
            stop("audio_format");
            return;
          }
          const audio = Buffer.from(event.delta, "base64");
          if (!audio.length || audio.length % 2) stop("audio_format");
          else sendAudio(audio);
        } else if (event.type === "session.closed") {
          finalized = true;
          const seconds = event.usage?.seconds;
          control({
            type: "closed",
            finalized: true,
            seconds: typeof seconds === "number" && Number.isFinite(seconds) && seconds >= 0 ? seconds : null,
          });
          clearTimeout(closeTimer);
          // Allow the final control message to flush before releasing sockets.
          device.close(1000);
          closeTimer = setTimeout(finish, 1000);
          upstream.close();
        } else if (event.type === "response.event" && event.event?.type === "response.completed") {
          const usage = event.event.response?.usage;
          const count = (value: unknown) =>
            typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;
          control({
            type: "backend_usage",
            input_tokens: count(usage?.input_tokens),
            output_tokens: count(usage?.output_tokens),
            cached_tokens: count(usage?.input_tokens_details?.cached_tokens),
          });
        } else if (event.type === "error") stop("live_api");
        // Transcripts and delegated response content are intentionally not logged.
      } catch {
        stop("upstream_protocol");
      }
    });
    upstream.on("unexpected-response", (_request, response) => {
      response.resume();
      control({ type: "error", code: "live_unavailable", status: response.statusCode });
      finish();
    });
    upstream.on("error", () => {
      control({ type: "error", code: "upstream_connection" });
      finish();
    });
    upstream.on("close", () => {
      if (!finalized) {
        control({ type: "closed", finalized: false });
        finish();
      }
    });
  });
}
