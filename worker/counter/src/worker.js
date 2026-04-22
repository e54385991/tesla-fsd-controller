// tesla-fsd-controller anonymous usage counter.
//
//   POST /ping  {id, version, env}  — records device for today (id = sha256(MAC) hex, 16-64 chars)
//   GET  /stats                      — returns {today, yesterday, month, year, total, date, byEnv, byVersion, byCountry}
//
// Storage (all keyed by device id so counting = listAll().length, inherently unique):
//   seen:YYYY-MM-DD:<id> → {v, e, c}   TTL 3d   — daily set, source of by-env/version/country aggregates
//   mo:YYYY-MM:<id>      → "1"         TTL 62d  — MAU set for calendar month
//   yr:YYYY:<id>         → "1"         TTL 400d — YAU set for calendar year
//   ever:<id>            → "1"         no TTL   — all-time unique devices
// Counts by listing prefixes — cheap enough for <10k daily devices,
// which is the ceiling we'd want before moving to a Durable Object counter.

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
};

const STATS_CACHE_TTL_SEC = 60;

export default {
  async fetch(request, env, ctx) {
    if (request.method === "OPTIONS") return new Response(null, { headers: CORS });

    const url = new URL(request.url);

    if (request.method === "POST" && url.pathname === "/ping") {
      return handlePing(request, env);
    }
    if (request.method === "GET" && url.pathname === "/stats") {
      return handleStatsCached(request, env, ctx);
    }
    return new Response(
      "tesla-counter\n\nPOST /ping {id, version, env}\nGET /stats\n",
      { headers: CORS }
    );
  },
};

async function handlePing(request, env) {
  let body;
  try { body = await request.json(); } catch { return txt("bad json", 400); }
  const { id, version, env: fwEnv } = body || {};
  if (typeof id !== "string" || !/^[a-f0-9]{16,64}$/.test(id)) return txt("bad id", 400);
  if (typeof version !== "string" || !/^[0-9a-zA-Z._-]{1,16}$/.test(version)) return txt("bad version", 400);
  if (typeof fwEnv !== "string" || !/^[a-z0-9_-]{1,32}$/.test(fwEnv)) return txt("bad env", 400);

  const today = todayStr();
  const seenKey = `seen:${today}:${id}`;
  if (await env.COUNTER.get(seenKey)) return txt("ok (dedup)");

  const month = today.slice(0, 7);
  const year = today.slice(0, 4);
  const moKey = `mo:${month}:${id}`;
  const yrKey = `yr:${year}:${id}`;
  const everKey = `ever:${id}`;

  // Probe the rollup keys so we only write the ones missing — avoids turning
  // one daily write into four and keeps us well under the 1k-writes/day free
  // tier. Reads are 100k/day, so GETs are cheap by comparison.
  const [hasMo, hasYr, hasEver] = await Promise.all([
    env.COUNTER.get(moKey),
    env.COUNTER.get(yrKey),
    env.COUNTER.get(everKey),
  ]);

  const country = request.headers.get("cf-ipcountry") || "??";
  const writes = [
    env.COUNTER.put(seenKey,
      JSON.stringify({ v: version, e: fwEnv, c: country }),
      { expirationTtl: 3 * 24 * 3600 }),
  ];
  if (!hasMo)   writes.push(env.COUNTER.put(moKey,   "1", { expirationTtl: 62 * 24 * 3600 }));
  if (!hasYr)   writes.push(env.COUNTER.put(yrKey,   "1", { expirationTtl: 400 * 24 * 3600 }));
  if (!hasEver) writes.push(env.COUNTER.put(everKey, "1"));
  await Promise.all(writes);
  return txt("ok");
}

async function handleStatsCached(request, env, ctx) {
  const cache = caches.default;
  const cacheKey = new Request(request.url, { method: "GET" });
  const hit = await cache.match(cacheKey);
  if (hit) {
    const resp = new Response(hit.body, { headers: new Headers(hit.headers) });
    resp.headers.set("X-Cache", "HIT");
    return resp;
  }
  const resp = await handleStats(env);
  // Write-through off the hot path so a slow/failed cache.put can't block or
  // 500 the response — /stats is an optimization, not a source of truth.
  ctx.waitUntil(cache.put(cacheKey, resp.clone()).catch(() => {}));
  resp.headers.set("X-Cache", "MISS");
  return resp;
}

async function handleStats(env) {
  const now = new Date();
  const today = dayStr(now);
  const yesterday = dayStr(new Date(now.getTime() - 86400_000));
  const month = today.slice(0, 7);
  const year = today.slice(0, 4);

  const [todayEntries, ydayEntries, monthEntries, yearEntries, everEntries] = await Promise.all([
    listAll(env.COUNTER, `seen:${today}:`),
    listAll(env.COUNTER, `seen:${yesterday}:`),
    listAll(env.COUNTER, `mo:${month}:`),
    listAll(env.COUNTER, `yr:${year}:`),
    listAll(env.COUNTER, `ever:`),
  ]);

  // Pull values for today to aggregate by env/version. Yesterday stays
  // count-only to keep the stats cheap.
  const byEnv = {};
  const byVersion = {};
  const byCountry = {};
  await Promise.all(
    todayEntries.map(async (k) => {
      const raw = await env.COUNTER.get(k.name);
      if (!raw) return;
      try {
        const { v, e, c } = JSON.parse(raw);
        if (e) byEnv[e] = (byEnv[e] || 0) + 1;
        if (v) byVersion[v] = (byVersion[v] || 0) + 1;
        if (c) byCountry[c] = (byCountry[c] || 0) + 1;
      } catch { /* skip malformed */ }
    })
  );

  return new Response(JSON.stringify({
    today: todayEntries.length,
    yesterday: ydayEntries.length,
    month: monthEntries.length,
    year: yearEntries.length,
    total: everEntries.length,
    date: today,
    byEnv,
    byVersion,
    byCountry,
  }), { headers: {
    ...CORS,
    "Content-Type": "application/json",
    "Cache-Control": `public, s-maxage=${STATS_CACHE_TTL_SEC}`,
  } });
}

async function listAll(KV, prefix) {
  let cursor, all = [];
  // Cap at 10 pages (10k keys) so a runaway day doesn't make /stats unbounded.
  for (let i = 0; i < 10; i++) {
    const r = await KV.list({ prefix, cursor, limit: 1000 });
    all = all.concat(r.keys);
    if (r.list_complete) break;
    cursor = r.cursor;
  }
  return all;
}

function todayStr() { return dayStr(new Date()); }
function dayStr(d) { return d.toISOString().slice(0, 10); }
function txt(s, status = 200) { return new Response(s, { status, headers: CORS }); }
