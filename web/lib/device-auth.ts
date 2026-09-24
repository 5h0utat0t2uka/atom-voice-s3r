import { createHash, timingSafeEqual } from "node:crypto";

export type DeviceOptions = {
  apiKey: string | undefined;
  deviceToken: string | undefined;
  checkLimit: (deviceId: string) => Promise<{ rateLimited: boolean; error?: string }>;
};

export function reply(body: object, status: number, extraHeaders: Record<string, string> = {}) {
  return Response.json(body, {
    status,
    headers: { "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff", ...extraHeaders },
  });
}

export function authenticateDevice(request: Request, options: DeviceOptions): string | Response {
  if (
    !options.apiKey ||
    !options.deviceToken ||
    options.deviceToken.length !== 64 ||
    !/^[a-f0-9]{64}$/.test(options.deviceToken)
  ) {
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
  if (request.headers.has("origin")) return reply({ error: "forbidden" }, 403);
  return createHash("sha256").update(options.deviceToken).digest("hex");
}

export async function limitDevice(deviceId: string, options: DeviceOptions): Promise<Response | undefined> {
  try {
    const limit = await options.checkLimit(deviceId);
    if (limit.error) return reply({ error: "rate_limit_unavailable" }, 503);
    if (limit.rateLimited) return reply({ error: "rate_limited" }, 429, { "Retry-After": "60" });
  } catch {
    return reply({ error: "rate_limit_unavailable" }, 503);
  }
}
