import { checkRateLimit } from "@vercel/firewall";

// Keep the existing Terraform rule ID; this quota limits GPT-Live session starts.
export async function checkDeviceLimit(deviceId: string) {
  if (process.env.VERCEL !== "1" || process.env.VERCEL_ENV !== "production" || process.env.NODE_ENV !== "production") {
    throw new Error("Vercel Firewall is required");
  }
  const host = process.env.VERCEL_PROJECT_PRODUCTION_URL;
  if (!host || !/^[a-zA-Z0-9.-]+$/.test(host)) throw new Error("Missing deployment host");
  return checkRateLimit("realtime-token", {
    headers: new Headers({ host }),
    rateLimitKey: deviceId,
  });
}
