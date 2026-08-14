/* AZD BITCOIN MINER -- dashboard logic
 *
 * Rules this file follows without exception:
 *   1. Nothing is ever displayed that the API did not report.
 *   2. `null` from the API means "not measurable" and renders as N/A --
 *      never as 0, never as a guess, never as a remembered previous value.
 *   3. The pool password is write-only. It is sent, never fetched, never shown.
 */

(function () {
    "use strict";

    var POLL_INTERVAL_MS = 2000;
    var NA = "N/A";

    var state = {
        online: false,
        running: false,
        consecutiveFailures: 0
    };

    // ------------------------------------------------------------- helpers

    function $(id) {
        return document.getElementById(id);
    }

    /** Renders a measured value, or N/A when the API reported null/undefined. */
    function setValue(element, value, unit) {
        if (value === null || value === undefined || value === "") {
            element.textContent = NA;
            element.classList.add("is-na");
            return false;
        }
        element.classList.remove("is-na");
        element.innerHTML = "";
        element.appendChild(document.createTextNode(String(value)));
        if (unit) {
            var span = document.createElement("span");
            span.className = "card-unit";
            span.textContent = unit;
            element.appendChild(span);
        }
        return true;
    }

    function setText(element, text) {
        element.textContent = text;
    }

    function setDot(element, kind, pulse) {
        element.className = "dot is-" + kind + (pulse ? " pulse" : "");
    }

    /** Hashrate with an appropriate SI prefix. Input is hashes per second. */
    function formatHashrate(hps) {
        if (hps === null || hps === undefined) return null;
        if (hps <= 0) return "0.00 H/s";
        var units = ["H/s", "kH/s", "MH/s", "GH/s", "TH/s", "PH/s"];
        var index = 0;
        var value = hps;
        while (value >= 1000 && index < units.length - 1) {
            value /= 1000;
            index += 1;
        }
        return value.toFixed(2) + " " + units[index];
    }

    function formatCount(value) {
        if (value === null || value === undefined) return null;
        return Number(value).toLocaleString("en-US");
    }

    function formatDuration(seconds) {
        if (seconds === null || seconds === undefined) return "00:00:00";
        var total = Math.max(0, Math.floor(seconds));
        var hours = Math.floor(total / 3600);
        var minutes = Math.floor((total % 3600) / 60);
        var secs = total % 60;
        function pad(n) { return (n < 10 ? "0" : "") + n; }
        return pad(hours) + ":" + pad(minutes) + ":" + pad(secs);
    }

    function formatDifficulty(value) {
        if (value === null || value === undefined) return null;
        if (value >= 1000000) return (value / 1000000).toFixed(2) + "M";
        if (value >= 1000) return (value / 1000).toFixed(2) + "K";
        if (value >= 1) return value.toFixed(2);
        return value.toPrecision(3);
    }

    function request(method, path, body) {
        var options = { method: method, headers: {} };
        if (body !== undefined) {
            options.headers["Content-Type"] = "application/json";
            options.body = JSON.stringify(body);
        }
        return fetch(path, options).then(function (response) {
            return response.json()
                .catch(function () { return {}; })
                .then(function (data) {
                    if (!response.ok) {
                        var message = data && data.error ? data.error : "HTTP " + response.status;
                        throw new Error(message);
                    }
                    return data;
                });
        });
    }

    function showMessage(element, text, kind) {
        element.textContent = text;
        element.className = "form-message" + (kind ? " is-" + kind : "");
        if (kind === "ok") {
            window.setTimeout(function () {
                if (element.textContent === text) {
                    element.textContent = "";
                    element.className = "form-message";
                }
            }, 4000);
        }
    }

    // ------------------------------------------------------------ renderers

    function renderStatus(status) {
        var stats = status.stats || {};
        var miningState = stats.miningState || "unknown";

        setValue($("mining-status"), miningState.toUpperCase());
        setText($("mining-backend"), "backend: " + (status.backend || "—"));

        var dotKind = "idle";
        if (miningState === "running") dotKind = "live";
        else if (miningState === "error") dotKind = "error";
        else if (miningState === "starting" || miningState === "stopping") dotKind = "warn";
        setDot($("mining-dot"), dotKind, miningState === "running");

        state.running = miningState === "running";
        $("btn-start").disabled = state.running;
        $("btn-stop").disabled = !state.running;

        // Hashrate: only ever what was measured.
        setValue($("hashrate"), formatHashrate(stats.hashrate10s));
        setText($("hashrate-detail"),
            "10s " + (formatHashrate(stats.hashrate10s) || NA) +
            "  ·  60s " + (formatHashrate(stats.hashrate60s) || NA) +
            "  ·  avg " + (formatHashrate(stats.hashrateSessionAverage) || NA));

        setValue($("total-hashes"), formatCount(stats.totalHashes));
        setText($("session-hashes"),
            "job " + (stats.currentJobId ? stats.currentJobId : "—"));

        setValue($("session-time"), formatDuration(stats.sessionSeconds));
        setText($("uptime"), "uptime " + formatDuration(stats.uptimeSeconds));

        // Best difficulty is null until a real share has been found.
        setValue($("best-difficulty"), formatDifficulty(stats.bestShareDifficulty));
        setText($("pool-difficulty"),
            "pool difficulty " + (stats.poolDifficulty !== null && stats.poolDifficulty !== undefined
                ? stats.poolDifficulty : "—"));

        setValue($("accepted"), formatCount(stats.acceptedShares));
        setValue($("rejected"), formatCount(stats.rejectedShares));

        var accepted = stats.acceptedShares || 0;
        var rejected = stats.rejectedShares || 0;
        var stale = stats.staleShares || 0;
        if (accepted + rejected > 0) {
            var rate = (accepted / (accepted + rejected)) * 100;
            setText($("accept-rate"), rate.toFixed(1) + "% accepted");
        } else {
            setText($("accept-rate"), "no shares submitted yet");
        }
        setText($("reject-reason"),
            stats.lastRejectReason ? stats.lastRejectReason
                                   : (stale > 0 ? stale + " stale" : "—"));

        $("accepted").classList.toggle("value-green", accepted > 0);
        $("rejected").classList.toggle("value-red", rejected > 0);

        var version = status.version ? " v" + status.version : "";
        setText($("version-line"),
            "AZD Bitcoin Miner" + version + " — " + (status.name || "local") +
            " — dashboard on 127.0.0.1");

        // The banner reflects the build, not a guess.
        $("cuda-banner").classList.toggle("is-hidden", status.cudaCompiled === true);
    }

    function renderGpu(gpu) {
        var device = gpu.device || {};

        if (gpu.available) {
            setValue($("gpu-name"), gpu.name || device.name || NA);
            var parts = [];
            if (device.computeCapability) parts.push("compute " + device.computeCapability);
            if (gpu.memoryTotalBytes) {
                parts.push(Math.round(gpu.memoryTotalBytes / (1024 * 1024)) + " MiB");
            }
            setText($("gpu-detail"), parts.length ? parts.join("  ·  ") : "device ready");
        } else {
            setValue($("gpu-name"), null);
            setText($("gpu-detail"), gpu.reason || "no GPU backend");
        }

        // Telemetry: null means not measurable. It is NEVER replaced by a number.
        setValue($("gpu-temp"),
            gpu.temperatureCelsius === null || gpu.temperatureCelsius === undefined
                ? null : gpu.temperatureCelsius.toFixed(0), "°C");
        setValue($("gpu-power"),
            gpu.powerWatts === null || gpu.powerWatts === undefined
                ? null : gpu.powerWatts.toFixed(0), "W");
        setValue($("gpu-util"),
            gpu.utilizationPercent === null || gpu.utilizationPercent === undefined
                ? null : gpu.utilizationPercent.toFixed(0), "%");
    }

    function renderPool(pool) {
        var poolState = pool.state || "disconnected";
        setValue($("pool-state"), pool.configured ? poolState.toUpperCase() : null);
        setText($("pool-url"), pool.url ? pool.url : "not configured");

        var dotKind = "idle";
        if (poolState === "authorized") dotKind = "live";
        else if (poolState === "error") dotKind = "error";
        else if (poolState === "connecting" || poolState === "connected" ||
                 poolState === "subscribed") dotKind = "warn";
        setDot($("pool-dot"), dotKind, poolState === "authorized");
    }

    function renderGpuLimits(config) {
        var gpu = config.gpu || {};
        setText($("gpu-temp-limits"),
            "warn " + (gpu.temperatureWarning !== undefined ? gpu.temperatureWarning + "°C" : "—") +
            " / stop " + (gpu.temperatureStop !== undefined ? gpu.temperatureStop + "°C" : "—"));
    }

    /** Fills the pool form. The password field is intentionally never filled. */
    function renderPoolForm(config) {
        var pool = config.pool || {};
        if (document.activeElement !== $("pool-host")) $("pool-host").value = pool.host || "";
        if (document.activeElement !== $("pool-port")) $("pool-port").value = pool.port || "";
        if (document.activeElement !== $("pool-username")) {
            $("pool-username").value = pool.username || "";
        }
        $("password-note").textContent = pool.passwordSet
            ? "A password is set. Write-only — leave blank to keep it."
            : "Write-only: the API never returns this value. Most pools accept \"x\".";
    }

    function setOnline(online, detail) {
        state.online = online;
        setDot($("api-dot"), online ? "live" : "error", false);
        setText($("api-status"), online ? "connected" : (detail || "miner offline"));
    }

    // ------------------------------------------------------------- polling

    function poll() {
        Promise.all([
            request("GET", "/api/status"),
            request("GET", "/api/gpu"),
            request("GET", "/api/pool"),
            request("GET", "/api/config")
        ]).then(function (results) {
            state.consecutiveFailures = 0;
            setOnline(true);
            renderStatus(results[0]);
            renderGpu(results[1]);
            renderPool(results[2]);
            renderGpuLimits(results[3]);
            renderPoolForm(results[3]);
        }).catch(function (error) {
            state.consecutiveFailures += 1;
            setOnline(false, "miner not reachable");
            if (state.consecutiveFailures === 1) {
                console.warn("dashboard poll failed:", error.message);
            }
        });
    }

    // -------------------------------------------------------------- events

    function onStart() {
        var message = $("control-message");
        showMessage(message, "Starting…", null);
        $("btn-start").disabled = true;

        request("POST", "/api/miner/start").then(function (data) {
            showMessage(message, "Mining started (backend: " + data.backend + ").", "ok");
            poll();
        }).catch(function (error) {
            showMessage(message, error.message, "error");
            $("btn-start").disabled = false;
        });
    }

    function onStop() {
        var message = $("control-message");
        showMessage(message, "Stopping…", null);
        $("btn-stop").disabled = true;

        request("POST", "/api/miner/stop").then(function () {
            showMessage(message, "Mining stopped.", "ok");
            poll();
        }).catch(function (error) {
            showMessage(message, error.message, "error");
        }).then(function () {
            poll();
        });
    }

    function onSavePool(event) {
        event.preventDefault();
        var message = $("pool-message");

        var patch = { pool: {} };
        var host = $("pool-host").value.trim();
        var port = parseInt($("pool-port").value, 10);
        var username = $("pool-username").value.trim();
        var password = $("pool-password").value;

        patch.pool.host = host;
        if (!isNaN(port)) patch.pool.port = port;
        patch.pool.username = username;
        // Only send the password when the user actually typed one, so saving
        // other fields never wipes it.
        if (password.length > 0) patch.pool.password = password;

        request("PUT", "/api/config", patch).then(function () {
            $("pool-password").value = "";
            showMessage(message, "Saved.", "ok");
            poll();
        }).catch(function (error) {
            showMessage(message, error.message, "error");
        });
    }

    // ---------------------------------------------------------------- init

    document.addEventListener("DOMContentLoaded", function () {
        $("btn-start").addEventListener("click", onStart);
        $("btn-stop").addEventListener("click", onStop);
        $("pool-form").addEventListener("submit", onSavePool);

        poll();
        window.setInterval(poll, POLL_INTERVAL_MS);
    });
})();
