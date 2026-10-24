/*
 * Direct editor downloads for docs/getting-started/downloads.md.
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
 * page retains its manual download and run instructions. Export templates are
 * rendered separately from the editor downloads. Failure points at Releases.
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
      // This picker renders above the page's build-flavor warning, and the
      // static download-flavor guard (tests/ci/check_download_build_flavor_warning.py)
      // cannot see JS-built links, so the card carries its own -O0 marker.
      flavor: "dev_build=yes, -O0: do not benchmark",
      flavorHref: "#linux-nightly-speed",
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
    var pathPrefix = "/" + repo + "/releases/download/";
    if (typeof url !== "string" || url.indexOf("https://github.com" + pathPrefix) !== 0) {
      return false;
    }
    try {
      // Browsers normalize literal and percent-encoded dot segments. A raw
      // prefix alone can therefore admit a link to another repository.
      var parsed = new URL(url);
      var decodedPath = decodeURIComponent(parsed.pathname);
      return parsed.origin === "https://github.com" &&
        parsed.pathname.indexOf(pathPrefix) === 0 &&
        decodedPath.indexOf(pathPrefix) === 0 &&
        !/%(?:2f|5c)/i.test(parsed.pathname) &&
        decodedPath.indexOf("%") === -1 &&
        !decodedPath.split("/").some(function (part) {
          return part === "." || part === ".." || part.indexOf("\\") !== -1;
        });
    } catch (e) {
      return false; // malformed URL or percent escape
    }
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

  function renderDownloads(container, repo, releases, now, fromCache) {
    var picks = pickPerPlatform(releases);
    var box = el("div");
    var grid = el("div");
    grid.className = "gs-download-grid";
    // The selector's platform order stays unchanged; editors alone are primary.
    [picks[1], picks[0]].forEach(function (p) {
      var os = p.platform.key === "windows-editor" ? "Windows" : "Linux";
      var card = el("section");
      card.className = "gs-download-card";
      card.appendChild(el("h3", os));
      var format = p.platform.ext === ".zip" ? "ZIP" : "TAR.XZ";
      var size = p.asset ? formatSize(p.asset.size) : "";
      var meta = el("p", "x86_64 · " + format + (size ? " · " + size : ""));
      meta.className = "gs-download-meta";
      card.appendChild(meta);
      if (p.platform.flavor) {
        var flavor = el("p");
        flavor.className = "gs-download-flavor";
        flavor.appendChild(link(p.platform.flavorHref, p.platform.flavor));
        card.appendChild(flavor);
      }
      if (!p.release) {
        card.appendChild(el("p", "No editor in the " + releases.length + " most recent releases."));
        card.appendChild(link(releasesPageUrl(repo), "Check release assets for " + os));
        grid.appendChild(card);
        return;
      }
      var build = el("p");
      build.className = "gs-download-meta";
      build.appendChild(el("code", p.release.tag));
      build.appendChild(document.createTextNode(p.release.prerelease ? " · Prerelease" : " · Release"));
      card.appendChild(build);
      var date = el("p", "Published ");
      date.className = "gs-download-meta";
      var time = el("time", isoDate(p.release.published));
      time.dateTime = isoDate(p.release.published);
      date.appendChild(time);
      card.appendChild(date);
      if (isSafeDownloadUrl(p.asset.url, repo)) {
        var download = link(p.asset.url, "Download for " + os, p.asset.name);
        download.className = "md-button md-button--primary gs-download-button";
        card.appendChild(download);
      } else {
        card.appendChild(el("p", "Download link unavailable."));
        card.appendChild(link(releasesPageUrl(repo), "Check release assets for " + os));
      }
      var verify = el("p");
      verify.className = "gs-download-meta";
      if (p.sidecar && isSafeDownloadUrl(p.sidecar.url, repo)) {
        verify.appendChild(link(p.sidecar.url, "SHA-256 checksum for " + os, p.sidecar.name));
      } else {
        verify.appendChild(document.createTextNode("Checksum unavailable; check release assets."));
      }
      card.appendChild(verify);
      var notes = staleNotes([p], releases, now);
      notes.forEach(function (n) {
        card.appendChild(el("p", n));
      });
      grid.appendChild(card);
    });
    box.appendChild(grid);
    var status = el("p", fromCache
      ? "Using the release list cached in this browser session (up to 10 minutes old)."
      : "Checked the " + releases.length + " most recent published releases. Each button downloads the newest available editor for that platform.");
    status.className = "gs-download-meta";
    box.appendChild(status);
    container.replaceChildren(box);
    container.hidden = false;

    var exportContainer = document.getElementById("gs-export-template-download");
    if (exportContainer) {
      var p = picks[2];
      var exportLink = el("p");
      if (p.release && isSafeDownloadUrl(p.asset.url, repo)) {
        exportLink.appendChild(link(p.asset.url, "Download Windows export template", p.asset.name));
        exportLink.appendChild(document.createTextNode(" · " + p.release.tag));
        if (p.sidecar && isSafeDownloadUrl(p.sidecar.url, repo)) {
          exportLink.appendChild(document.createTextNode(" · "));
          exportLink.appendChild(link(p.sidecar.url, "SHA-256 checksum", p.sidecar.name));
        }
      } else {
        exportLink.appendChild(link(releasesPageUrl(repo), "Find Windows export templates in release assets"));
      }
      var exportNotes = staleNotes([p], releases, now);
      exportContainer.replaceChildren(exportLink);
      exportNotes.forEach(function (n) { exportContainer.appendChild(el("p", n)); });
      exportContainer.hidden = false;
    }
  }

  function renderFailure(container, repo) {
    var p = el("p");
    var em = el("em");
    em.appendChild(document.createTextNode("Couldn't load editor downloads. "));
    em.appendChild(link(releasesPageUrl(repo), "Open GitHub Releases"));
    em.appendChild(document.createTextNode(" and use the manual download instructions below."));
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
    var loading = el("p", "Finding editor downloads…");
    loading.setAttribute("role", "status");
    container.replaceChildren(loading);
    container.hidden = false;

    var now = Date.now();
    var cached = readCache(repo, now);
    if (cached) {
      renderDownloads(container, repo, cached, now, true);
      container.setAttribute("data-gs-state", "live");
      return;
    }
    fetchReleases(repo).then(
      function (releases) {
        writeCache(repo, releases, now);
        if (document.body.contains(container)) {
          renderDownloads(container, repo, releases, now, false);
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
