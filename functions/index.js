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
// All functions pinned to us-central1 to match the RTDB region.
const db = functions.region("us-central1").database;

// Irrigation complete
exports.alertIrrigationComplete = db
  .ref("/{date}/Irrigation/Complete/{time}")
  .onCreate((snap, ctx) =>
    notify("Irrigation Complete", `Finished at ${ctx.params.time} -- ${snap.val()} L delivered`)
  );
