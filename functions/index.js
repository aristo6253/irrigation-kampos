const functions = require("firebase-functions");
const https     = require("https");

// ── CONFIG ───────────────────────────────────────────────────────────────────
const NTFY_TOPIC = "irrigation-kampos-86760"; // must match index.html

// ── HELPER ───────────────────────────────────────────────────────────────────
function notify(title, body, priority = "default") {
  return new Promise((resolve, reject) => {
    const data = Buffer.from(body, "utf8");
    const req  = https.request({
      hostname: "ntfy.sh",
      path:     `/${NTFY_TOPIC}`,
      method:   "POST",
      headers: {
        "Title":          title,
        "Priority":       priority,
        "Tags":           "seedling",
        "Content-Type":   "text/plain",
        "Content-Length": data.length,
      },
    }, res => {
      res.resume(); // drain response
      if (res.statusCode >= 200 && res.statusCode < 300) resolve();
      else reject(new Error(`ntfy status ${res.statusCode}`));
    });
    req.on("error", reject);
    req.write(data);
    req.end();
  });
}

// ── TRIGGERS ─────────────────────────────────────────────────────────────────
// All functions pinned to europe-west1 to match the RTDB region.
const db = functions.region("europe-west1").database;

// Faulty valve — urgent
exports.alertFaultyValve = db
  .ref("/{date}/FaultyValve/{time}")
  .onCreate((snap, ctx) =>
    notify("Faulty Valve", `Valve ${snap.val()} reported faulty (${ctx.params.date} ${ctx.params.time})`, "urgent")
  );

// No flow detected — high
exports.alertNoFlow = db
  .ref("/{date}/Checked-NoFlow/{time}")
  .onCreate((snap, ctx) =>
    notify("No Flow Detected", `No flow on valve ${snap.val()} at ${ctx.params.time}`, "high")
  );

// Irrigation started
exports.alertWaterOn = db
  .ref("/{date}/WaterOn/{time}")
  .onCreate((_snap, ctx) =>
    notify("Irrigation Started", `Watering began at ${ctx.params.time} on ${ctx.params.date}`)
  );

// Irrigation complete
exports.alertIrrigationComplete = db
  .ref("/{date}/Irrigation/Complete/{time}")
  .onCreate((snap, ctx) =>
    notify("Irrigation Complete", `Finished at ${ctx.params.time} -- ${snap.val()} L delivered`)
  );

// Low battery — high priority
exports.alertLowBattery = db
  .ref("/{date}/batteryvoltage/{time}")
  .onCreate((snap, ctx) => {
    const v = parseFloat(snap.val());
    if (v < 11) {
      return notify("Low Battery", `Voltage at ${ctx.params.time}: ${v} V`, "high");
    }
    return null;
  });

// Very long sleep (>24 h) — low priority
exports.alertLongSleep = db
  .ref("/{date}/WentToSleep/{time}/toWakeUpInMins")
  .onCreate((snap, ctx) => {
    const mins = parseInt(snap.val(), 10);
    if (mins > 1440) {
      return notify("Long Sleep", `Device sleeping for ~${Math.round(mins / 60)} h on ${ctx.params.date}`, "low");
    }
    return null;
  });
