import { authenticateDevice, type DeviceOptions, limitDevice, reply } from "./device-auth.ts";

export const realtimeModel = "gpt-realtime-2.1";
export const sessionConfiguration = {
  type: "realtime",
  model: realtimeModel,
  output_modalities: ["audio"],
  instructions: "日本語で明瞭に、短い一文で返答してください。",
  max_output_tokens: 256,
  audio: {
    input: { format: { type: "audio/pcm", rate: 24000 }, turn_detection: null },
    output: { format: { type: "audio/pcm", rate: 24000 }, voice: "marin" },
  },
};

// Limit memory before parsing an upstream response. Never log tokens or API error bodies.
async function readJson(response: Response): Promise<unknown> {
  const reader = response.body?.getReader();
  if (!reader) throw new Error("Missing response");
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > 16384) throw new Error("Response too large");
      chunks.push(value);
    }
  } finally {
    await reader.cancel();
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8"));
}

export async function issueRealtimeToken(request: Request, options: DeviceOptions): Promise<Response> {
  if (request.method !== "POST") return reply({ error: "method_not_allowed" }, 405, { Allow: "POST" });
  const deviceId = authenticateDevice(request, options);
  if (deviceId instanceof Response) return deviceId;
  if (request.body !== null) {
    const reader = request.body.getReader();
    try {
      const first = await reader.read();
      if (!first.done) return reply({ error: "body_not_allowed" }, 400);
    } finally {
      await reader.cancel();
    }
  }
  const limited = await limitDevice(deviceId, options);
  if (limited) return limited;
  try {
    const upstream = await fetch("https://api.openai.com/v1/realtime/client_secrets", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${options.apiKey}`,
        "Content-Type": "application/json",
        "OpenAI-Safety-Identifier": deviceId,
      },
      body: JSON.stringify({ expires_after: { anchor: "created_at", seconds: 60 }, session: sessionConfiguration }),
      cache: "no-store",
      redirect: "error",
      signal: AbortSignal.timeout(10000),
    });
    if (!upstream.ok) {
      await upstream.body?.cancel();
      return reply({ error: "token_issuance_failed" }, 502);
    }
    const data = await readJson(upstream);
    if (typeof data !== "object" || data === null || !("value" in data) || !("expires_at" in data)) {
      return reply({ error: "invalid_upstream_response" }, 502);
    }
    if (
      typeof data.value !== "string" ||
      !/^ek_[A-Za-z0-9_-]{1,500}$/.test(data.value) ||
      typeof data.expires_at !== "number" ||
      !Number.isSafeInteger(data.expires_at) ||
      data.expires_at <= Math.floor(Date.now() / 1000)
    ) {
      return reply({ error: "invalid_upstream_response" }, 502);
    }
    return reply({ value: data.value, expires_at: data.expires_at, model: realtimeModel }, 200);
  } catch {
    return reply({ error: "token_issuance_failed" }, 502);
  }
}
