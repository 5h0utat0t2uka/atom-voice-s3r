import { createHash, timingSafeEqual } from "node:crypto";

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

type TokenOptions = {
  apiKey: string | undefined;
  deviceToken: string | undefined;
  checkLimit: (deviceId: string) => Promise<{ rateLimited: boolean; error?: string }>;
};

function reply(body: object, status: number, extraHeaders: Record<string, string> = {}) {
  return Response.json(body, {
    status,
    headers: { "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff", ...extraHeaders },
  });
}

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

export async function issueRealtimeToken(request: Request, options: TokenOptions): Promise<Response> {
  if (request.method !== "POST") return reply({ error: "method_not_allowed" }, 405, { Allow: "POST" });
  if (!options.apiKey || options.deviceToken?.length !== 64 || !/^[a-f0-9]{64}$/.test(options.deviceToken)) {
    return reply({ error: "server_not_configured" }, 503);
  }
  const authorization = request.headers.get("authorization") ?? "";
  const expected = `Bearer ${options.deviceToken}`;
  if (
    authorization.length !== expected.length ||
    !/^Bearer [a-f0-9]{64}$/.test(authorization) ||
    !timingSafeEqual(Buffer.from(authorization), Buffer.from(expected))
  ) {
    return reply({ error: "unauthorized" }, 401);
  }
  // This endpoint is for the device, not cross-origin browser requests or client-supplied session settings.
  if (request.headers.has("origin")) return reply({ error: "forbidden" }, 403);
  if (request.body !== null) {
    const reader = request.body.getReader();
    try {
      const first = await reader.read();
      if (!first.done) return reply({ error: "body_not_allowed" }, 400);
    } finally {
      await reader.cancel();
    }
  }
  const deviceId = createHash("sha256").update(options.deviceToken).digest("hex");
  try {
    const limit = await options.checkLimit(deviceId);
    if (limit.error) return reply({ error: "rate_limit_unavailable" }, 503);
    if (limit.rateLimited) return reply({ error: "rate_limited" }, 429, { "Retry-After": "60" });
  } catch {
    return reply({ error: "rate_limit_unavailable" }, 503);
  }
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
