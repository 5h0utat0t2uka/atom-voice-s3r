import { checkDeviceLimit } from "@/lib/device-limit";
import { issueRealtimeToken } from "@/lib/realtime-token";

export const runtime = "nodejs";
export const maxDuration = 20;

export async function POST(request: Request) {
  return issueRealtimeToken(request, {
    apiKey: process.env.OPENAI_API_KEY,
    deviceToken: process.env.DEVICE_TOKEN,
    checkLimit: checkDeviceLimit,
  });
}
