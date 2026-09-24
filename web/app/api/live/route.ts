import { experimental_upgradeWebSocket } from "@vercel/functions";
import { checkDeviceLimit } from "@/lib/device-limit";
import { authorizeLive, relayLive } from "@/lib/live";

export const runtime = "nodejs";
export const maxDuration = 300;

export async function GET(request: Request) {
  const apiKey = process.env.OPENAI_API_KEY ?? "";
  const authorization = await authorizeLive(request, {
    apiKey,
    deviceToken: process.env.DEVICE_TOKEN,
    checkLimit: checkDeviceLimit,
  });
  if (authorization instanceof Response) return authorization;
  return experimental_upgradeWebSocket((device) => relayLive(device, apiKey, authorization), { maxPayload: 2048 });
}
