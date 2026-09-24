// Exercise the same public endpoint as the device. Never print credentials.
// --rate-limit issues up to six client secrets but does not generate audio.
import assert from "node:assert/strict";
import { setTimeout } from "node:timers/promises";

async function verify() {
  const endpoint = new URL(process.env.REALTIME_TOKEN_URL ?? "");
  const deviceToken = process.env.DEVICE_TOKEN ?? "";
  if (
    endpoint.protocol !== "https:" ||
    endpoint.pathname !== "/api/realtime/token" ||
    endpoint.username ||
    endpoint.password ||
    !/^[a-f0-9]{64}$/.test(deviceToken)
  ) {
    throw new Error("Check REALTIME_TOKEN_URL and DEVICE_TOKEN in .enc.env.");
  }
  const request = (authenticated) =>
    fetch(endpoint, {
      method: "POST",
      headers: authenticated ? { Authorization: `Bearer ${deviceToken}` } : {},
      redirect: "error",
      signal: AbortSignal.timeout(20000),
    });
  const unauthorized = await request(false);
  await unauthorized.body?.cancel();
  assert.equal(unauthorized.status, 401, "Requests without device authentication must be rejected");
  console.log("Unauthenticated request: 401.");

  const testLimit = process.argv.includes("--rate-limit");
  if (testLimit) {
    // Begin just after the next fixed window starts. Do not use the device concurrently.
    const wait = 60000 - (Date.now() % 60000) + 1000;
    console.log(`Waiting ${Math.ceil(wait / 1000)} seconds for a fresh rate-limit window…`);
    await setTimeout(Math.min(wait, 60000));
    if (wait > 60000) await setTimeout(wait - 60000);
  }
  const started = Date.now();
  for (let i = 1; i <= (testLimit ? 7 : 1); i++) {
    const response = await request(true);
    const expected = i <= 6 ? 200 : 429;
    assert.equal(response.status, expected, `Request ${i}: expected HTTP ${expected}`);
    assert.match(response.headers.get("cache-control") ?? "", /no-store/);
    const data = await response.json();
    if (expected === 200) {
      assert.equal(typeof data.value, "string", "Client secret is missing");
      assert.ok(data.value.startsWith("ek_"), "Client secret has an unexpected format");
      assert.ok(data.expires_at > Date.now() / 1000, "Client secret has expired");
    } else {
      assert.equal(data.error, "rate_limited");
    }
    console.log(`Authenticated request ${i}: ${response.status} (secret not displayed).`);
  }
  if (testLimit) assert.ok(Date.now() - started < 59000, "Test crossed a rate-limit window; results are inconclusive");
  console.log("Verification passed. No audio was generated.");
}

verify().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
