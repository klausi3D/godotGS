/*
 * Live "newest build per platform" table for docs/getting-started/downloads.md.
 *
 * Nightlies are published Linux-only whenever the Windows lane fails, so the
 * newest nightly is not necessarily the newest Windows build. Hand-written
 * prose about which nightly has which asset goes stale the next morning, and a
 * build-time query would go stale too (the docs deploy only on docs-path
 * pushes). This script therefore asks the GitHub Releases API from the
 * reader's browser and, per platform, links the newest release that actually
 * ships that platform's asset.
 *
 * Progressive enhancement only: the placeholder is empty and `hidden` in the
 * page source, so with JS off, the API rate-limited (403/429) or offline the
 * page reads exactly as its static prose and Releases link. Failure shows one
 * quiet line pointing at Releases, nothing more.
 *
 * Deliberately NOT `releases/latest`: that endpoint skips prereleases, and
 * every nightly is a prerelease.
 *
 * All API data reaches the DOM through textContent or a validated href; no
 * innerHTML, no tokens.
 */
(function () {
  "use strict";

  var CONTAINER_ID = "gs-latest-nightlies";
  var PER_PAGE = 20;
  var TIMEOUT_MS = 8000;
  var CACHE_TTL_MS = 10 * 60 * 1000;
  var REPO_RE = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;

  // Asset-name patterns, verified against the published assets of
  // nightly-20260926 (Linux + Windows) and nightly-20260930 (Linux-only), and
  // against the archive_base names in .github/workflows/release_builds.yml.
  var PLATFORMS = [
    {
      key: "linux-editor",
      label: "Linux editor",
      // This table renders above the page's build-flavor warning, and the
      // static download-flavor guard (tests/ci/check_download_build_flavor_warning.py)
      // cannot see JS-built links, so the row carries its own -O0 marker.
      flavor: "dev_build=yes, -O0: do not benchmark",
      flavorHref: "#build-flavor-and-performance",
      pattern: /^godotgs-linux-x86_64-.+\.tar\.xz$/,
      ext: ".tar.xz",
    },
    {
      key: "windows-editor",
      label: "Windows editor",
      pattern: /^godotgs-windows-x86_64-.+\.zip$/,
      ext: ".zip",
    },
    {
      key: "windows-export-template",
      label: "Windows export template",
      pattern: /^godotgs-export-template-windows-x86_64-.+\.zip$/,
      ext: ".zip",
    },
  ];

  var DAY_MS = 24 * 60 * 60 * 1000;

  function releasesPageUrl(repo) {
    return "https://github.com/" + repo + "/releases";
  }

  function isSafeDownloadUrl(url, repo) {
    return (
      typeof url === "string" &&
      url.indexOf("https://github.com/" + repo + "/releases/download/") === 0
    );
  }

  /* Reduce the API payload to the fields this page uses (also what is cached). */
  function normalizeReleases(payload) {
    if (!Array.isArray(payload)) {
      return null;
    }
    var out = [];
    for (var i = 0; i < payload.length; i++) {
      var r = payload[i];
      if (!r || r.draft || typeof r.tag_name !== "string") {
        continue;
      }
      var published = Date.parse(r.published_at);
      if (isNaN(published)) {
        continue;
      }
      var assets = [];
      var raw = Array.isArray(r.assets) ? r.assets : [];
      for (var j = 0; j < raw.length; j++) {
        var a = raw[j];
        if (a && typeof a.name === "string") {
          assets.push({
            name: a.name,
            url: a.browser_download_url,
            size: typeof a.size === "number" ? a.size : null,
          });
        }
      }
      out.push({
        tag: r.tag_name,
        published: published,
        prerelease: !!r.prerelease,
        assets: assets,
      });
    }
    // Newest first, by publish time rather than trusting API order.
    out.sort(function (x, y) {
      return y.published - x.published;
    });
    return out;
  }

  function findAsset(release, pattern) {
    for (var i = 0; i < release.assets.length; i++) {
      if (pattern.test(release.assets[i].name)) {
        return release.assets[i];
      }
    }
    return null;
  }

  function findSidecar(release, asset, ext) {
    var base = asset.name.slice(0, asset.name.length - ext.length);
    for (var i = 0; i < release.assets.length; i++) {
      if (release.assets[i].name === base + ".sha256") {
        return release.assets[i];
      }
    }
    return null;
  }

  /*
   * For each platform: the newest release carrying its asset, and how many
   * newer releases were published without it. `releases` must be normalized.
   */
  function pickPerPlatform(releases) {
    return PLATFORMS.map(function (platform) {
      for (var i = 0; i < releases.length; i++) {
        var asset = findAsset(releases[i], platform.pattern);
        if (asset) {
          return {
            platform: platform,
            release: releases[i],
            asset: asset,
            sidecar: findSidecar(releases[i], asset, platform.ext),
            newerWithout: i,
          };
        }
      }
      return {
        platform: platform,
        release: null,
        asset: null,
        sidecar: null,
        newerWithout: releases.length,
      };
    });
  }

  function isoDate(ms) {
    return new Date(ms).toISOString().slice(0, 10);
  }

  function relativeDays(ms, now) {
    var days = Math.floor((now - ms) / DAY_MS);
    if (days <= 0) {
      return "today";
    }
    return days === 1 ? "1 day ago" : days + " days ago";
  }

  function formatSize(bytes) {
    if (typeof bytes !== "number" || bytes <= 0) {
      return "";
    }
    var mb = bytes / (1024 * 1024);
    return mb >= 10 ? Math.round(mb) + " MB" : mb.toFixed(1) + " MB";
  }

  function allNightlies(releases) {
    for (var i = 0; i < releases.length; i++) {
      if (releases[i].tag.indexOf("nightly-") !== 0) {
        return false;
      }
    }
    return true;
  }

  /* Plain-text notes for platforms the newest release does not carry. */
  function staleNotes(picks, releases, now) {
    var nightly = allNightlies(releases);
    var notes = [];
    picks.forEach(function (p) {
      if (p.newerWithout === 0) {
        return;
      }
      if (!p.release) {
        notes.push(
          p.platform.label +
            ": not in any of the " +
            releases.length +
            " most recent releases."
        );
        return;
      }
      var n = p.newerWithout;
      var noun = nightly
        ? n === 1 ? "nightly" : "nightlies"
        : n === 1 ? "release" : "releases";
      var newer =
        n === 1 ? "the newer " + noun + " was" : "the " + n + " newer " + noun + " were";
      notes.push(
        p.platform.label +
          ": last built in " +
          p.release.tag +
          " (" +
          relativeDays(p.release.published, now) +
          "); " +
          newer +
          " published without it."
      );
    });
    return notes;
  }

  /* ---------------------------------------------------------------- DOM -- */

  function el(tag, text) {
    var node = document.createElement(tag);
    if (text !== undefined) {
      node.textContent = text;
    }
    return node;
  }

  function link(href, text, title) {
    var a = el("a", text);
    a.href = href;
    if (title) {
      a.title = title;
    }
    return a;
  }

  // Dates and file names must not break at their hyphens; set via the DOM so
  // no stylesheet is needed for one table.
  function nowrapCell() {
    var td = el("td");
    td.style.whiteSpace = "nowrap";
    return td;
  }

  function renderTable(container, repo, releases, now) {
    var picks = pickPerPlatform(releases);

    var box = el("div");
    box.className = "admonition info";
    var title = el("p", "Newest build per platform");
    title.className = "admonition-title";
    box.appendChild(title);
    box.appendChild(
      el(
        "p",
        "Fetched live from the GitHub Releases API when this page loaded. " +
          "Each row links the newest release that actually contains that " +
          "file (all builds are x86_64)."
      )
    );

    var table = el("table");
    var thead = el("thead");
    var headRow = el("tr");
    ["Platform", "Newest build", "Published", "Download", "SHA-256"].forEach(
      function (h) {
        headRow.appendChild(el("th", h));
      }
    );
    thead.appendChild(headRow);
    table.appendChild(thead);

    var tbody = el("tbody");
    picks.forEach(function (p) {
      var tr = el("tr");
      var platformCell = el("td", p.platform.label);
      if (p.platform.flavor) {
        platformCell.appendChild(el("br"));
        var flavorNote = el("small");
        flavorNote.appendChild(link(p.platform.flavorHref, p.platform.flavor));
        platformCell.appendChild(flavorNote);
      }
      tr.appendChild(platformCell);
      if (!p.release) {
        var none = el("td");
        none.colSpan = 4;
        none.appendChild(
          document.createTextNode(
            "No build in the " + releases.length + " most recent releases; see "
          )
        );
        none.appendChild(link(releasesPageUrl(repo), "Releases"));
        none.appendChild(document.createTextNode("."));
        tr.appendChild(none);
        tbody.appendChild(tr);
        return;
      }
      var tagCell = nowrapCell();
      tagCell.appendChild(el("code", p.release.tag));
      tr.appendChild(tagCell);

      var dateCell = nowrapCell();
      var time = el("time", isoDate(p.release.published));
      time.dateTime = isoDate(p.release.published);
      dateCell.appendChild(time);
      dateCell.appendChild(el("br"));
      dateCell.appendChild(
        document.createTextNode(relativeDays(p.release.published, now))
      );
      tr.appendChild(dateCell);

      var dl = nowrapCell();
      if (isSafeDownloadUrl(p.asset.url, repo)) {
        dl.appendChild(link(p.asset.url, p.platform.ext, p.asset.name));
        var size = formatSize(p.asset.size);
        if (size) {
          dl.appendChild(el("br"));
          dl.appendChild(document.createTextNode(size));
        }
      } else {
        dl.textContent = "—";
      }
      tr.appendChild(dl);

      var sha = nowrapCell();
      if (p.sidecar && isSafeDownloadUrl(p.sidecar.url, repo)) {
        sha.appendChild(link(p.sidecar.url, ".sha256", p.sidecar.name));
      } else {
        sha.textContent = "—";
      }
      tr.appendChild(sha);
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);

    var wrap = el("div");
    wrap.className = "md-typeset__scrollwrap";
    var inner = el("div");
    inner.className = "md-typeset__table";
    inner.appendChild(table);
    wrap.appendChild(inner);
    box.appendChild(wrap);

    var notes = staleNotes(picks, releases, now);
    if (notes.length) {
      var ul = el("ul");
      notes.forEach(function (n) {
        ul.appendChild(el("li", n));
      });
      box.appendChild(ul);
    }

    container.replaceChildren(box);
    container.hidden = false;
  }

  function renderFailure(container, repo) {
    var p = el("p");
    var em = el("em");
    em.appendChild(document.createTextNode("Couldn't load the live list; see "));
    em.appendChild(link(releasesPageUrl(repo), "Releases"));
    em.appendChild(document.createTextNode("."));
    p.appendChild(em);
    container.replaceChildren(p);
    container.hidden = false;
  }

  /* ------------------------------------------------------------ network -- */

  function cacheKey(repo) {
    return "gs-latest-nightlies:v1:" + repo;
  }

  function readCache(repo, now) {
    try {
      var raw = window.sessionStorage.getItem(cacheKey(repo));
      if (!raw) {
        return null;
      }
      var entry = JSON.parse(raw);
      if (entry && Array.isArray(entry.releases) && now - entry.t < CACHE_TTL_MS) {
        return entry.releases;
      }
    } catch (e) {
      /* storage unavailable or corrupt: fetch instead */
    }
    return null;
  }

  function writeCache(repo, releases, now) {
    try {
      window.sessionStorage.setItem(
        cacheKey(repo),
        JSON.stringify({ t: now, releases: releases })
      );
    } catch (e) {
      /* storage unavailable: nothing to do */
    }
  }

  function fetchReleases(repo) {
    var url =
      "https://api.github.com/repos/" + repo + "/releases?per_page=" + PER_PAGE;
    var controller =
      typeof AbortController === "function" ? new AbortController() : null;
    var timer = controller
      ? setTimeout(function () {
          controller.abort();
        }, TIMEOUT_MS)
      : null;
    return fetch(url, {
      headers: { Accept: "application/vnd.github+json" },
      credentials: "omit",
      signal: controller ? controller.signal : undefined,
    })
      .then(function (res) {
        // 403/429 (rate limit) and every other non-2xx fall back quietly.
        if (!res.ok) {
          throw new Error("HTTP " + res.status);
        }
        return res.json();
      })
      .then(function (json) {
        var releases = normalizeReleases(json);
        if (!releases || releases.length === 0) {
          throw new Error("no releases");
        }
        return releases;
      })
      .finally(function () {
        if (timer) {
          clearTimeout(timer);
        }
      });
  }

  function init() {
    var container = document.getElementById(CONTAINER_ID);
    if (!container || container.getAttribute("data-gs-state")) {
      return;
    }
    var repo = container.getAttribute("data-repo") || "";
    if (!REPO_RE.test(repo) || typeof fetch !== "function") {
      return; // static prose stays the whole story
    }
    container.setAttribute("data-gs-state", "loading");

    var now = Date.now();
    var cached = readCache(repo, now);
    if (cached) {
      renderTable(container, repo, cached, now);
      container.setAttribute("data-gs-state", "live");
      return;
    }
    fetchReleases(repo).then(
      function (releases) {
        writeCache(repo, releases, now);
        if (document.body.contains(container)) {
          renderTable(container, repo, releases, now);
          container.setAttribute("data-gs-state", "live");
        }
      },
      function () {
        if (document.body.contains(container)) {
          renderFailure(container, repo);
          container.setAttribute("data-gs-state", "failed");
        }
      }
    );
  }

  if (typeof module !== "undefined" && module.exports) {
    // Node-only hook for exercising the pure selection logic outside a browser.
    module.exports = {
      PLATFORMS: PLATFORMS,
      normalizeReleases: normalizeReleases,
      pickPerPlatform: pickPerPlatform,
      staleNotes: staleNotes,
      relativeDays: relativeDays,
      isSafeDownloadUrl: isSafeDownloadUrl,
    };
  }

  if (typeof document === "undefined") {
    return;
  }
  // Material's instant navigation swaps page content without a reload;
  // document$ emits on the first load and after every such swap.
  if (typeof window.document$ !== "undefined" && window.document$.subscribe) {
    window.document$.subscribe(init);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
