import { checkRateLimit } from "@vercel/firewall";
import { issueRealtimeToken } from "@/lib/realtime-token";

export const runtime = "nodejs";
export const maxDuration = 20;

export async function POST(request: Request) {
  return issueRealtimeToken(request, {
    apiKey: process.env.OPENAI_API_KEY,
    deviceToken: process.env.DEVICE_TOKEN,
    checkLimit: async (deviceId) => {
      // The SDK allows requests in development without a rule. Fail closed instead.
      if (
        process.env.VERCEL !== "1" ||
        process.env.VERCEL_ENV !== "production" ||
        process.env.NODE_ENV !== "production"
      ) {
        throw new Error("Vercel Firewall is required");
      }
      // Generated deployment URLs can require Vercel login even for production deployments.
      const host = process.env.VERCEL_PROJECT_PRODUCTION_URL;
      if (!host || !/^[a-zA-Z0-9.-]+$/.test(host)) throw new Error("Missing deployment host");
      // Do not forward Authorization or trust the request's Host for the SDK's outgoing fetch.
      return checkRateLimit("realtime-token", {
        headers: new Headers({ host }),
        rateLimitKey: deviceId,
      });
    },
  });
}
