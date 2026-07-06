#!/usr/bin/env python3
"""Processes a "Game compatibility report" issue end-to-end: validates the
submitted APK link, downloads it, extracts an icon, checks the Play Store
category, runs an automated pass over the three submitted logs, commits the
result to the AndroidHorizon/compat-reports repo (overwriting any older
submission for the same package+version), and closes+locks the issue with a
summary. Untrusted issue content (body, user) only ever flows through
os.environ / file writes / git — never through a shell string — so it can't
be used for command injection.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

VERDICT_LABEL = {
    "ok":      "✅ Playable",
    "issues":  "⚠️ Runs with issues",
    "fail":    "❌ Fails to launch",
    "unknown": "❓ Inconclusive",
}

LABEL_MAP = {
    "Upload the APK": "apk_url",
    "Where did this APK come from?": "source_site",
    "launcher_log.txt": "launcher_log",
    "compat_log.txt": "compat_log",
    "log.txt (Translation Core)": "core_log",
    "Anything else worth knowing?": "notes",
}

REQUIRED_FIELDS = [
    "apk_url", "source_site", "launcher_log", "compat_log", "core_log",
]

STALL_RE = re.compile(r'(stall|STALL\(severe\)): frame (\d+) stalled for (\d+)ms')
FRAME_EVIDENCE_RE = re.compile(r'\bframe\s+\d+\b', re.IGNORECASE)
ERROR_MARKERS = [
    "abort", "fatal", "unresolved symbol", "font load failed",
    "envsetnextload failed", "core nro missing", "segmentation fault",
    "unhandled exception", "software closed", "failed to load",
]
DENSITY_ORDER = ["xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", "ldpi", "anydpi", "nodpi"]


# ---------------------------------------------------------------- parsing --

def parse_issue_body(body: str) -> dict:
    """GitHub issue forms render each field as `### Label` followed by the
    answer (textareas with render: set get the answer inside a fenced code
    block) — split on that heading structure."""
    fields, current_label, current_lines = {}, None, []

    def flush():
        if current_label is not None:
            fields[current_label] = "\n".join(current_lines).strip("\n")

    for line in body.split("\n"):
        if line.startswith("### "):
            flush()
            current_label = line[4:].strip()
            current_lines = []
        else:
            current_lines.append(line)
    flush()

    cleaned = {}
    for label, value in fields.items():
        v = value.strip()
        if v == "_No response_":
            cleaned[label] = ""
            continue
        vlines = value.split("\n")
        if len(vlines) >= 2 and vlines[0].strip().startswith("```") and vlines[-1].strip() == "```":
            value = "\n".join(vlines[1:-1])
        cleaned[label] = value.strip("\n")
    return cleaned


def safe_path_component(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.\-]", "_", s.strip())[:100] or "unknown"


APK_URL_RE = re.compile(r'https?://[^\s()<>\[\]]+\.apk\b', re.IGNORECASE)


def extract_apk_url(field_text: str):
    """The "Upload the APK" field is either a plain pasted link, or GitHub's
    auto-generated attachment markdown (`[game.apk](https://github.com/
    user-attachments/files/.../game.apk)`) from dragging the file in — either
    way, pull out the first URL that actually ends in .apk."""
    m = APK_URL_RE.search(field_text)
    return m.group(0) if m else None


# --------------------------------------------------------------- GitHub API --

def gh_api(method, path, token, payload=None):
    url = f"https://api.github.com{path}"
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github+json",
        "User-Agent": "android-horizon-compat-bot",
        "Content-Type": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def post_comment(repo, issue_number, token, body):
    gh_api("POST", f"/repos/{repo}/issues/{issue_number}/comments", token, {"body": body})


def add_label(repo, issue_number, token, label):
    gh_api("POST", f"/repos/{repo}/issues/{issue_number}/labels", token, {"labels": [label]})


def close_and_lock(repo, issue_number, token):
    gh_api("PATCH", f"/repos/{repo}/issues/{issue_number}", token,
           {"state": "closed", "state_reason": "completed"})
    gh_api("PUT", f"/repos/{repo}/issues/{issue_number}/lock", token, {"lock_reason": "resolved"})


# ------------------------------------------------------------------ steps --

def download_apk(url, dest, max_bytes=300 * 1024 * 1024, timeout=180):
    try:
        result = subprocess.run(
            ["curl", "-sL", "--max-time", str(timeout), "--max-filesize", str(max_bytes),
             "-w", "%{http_code}", "-o", dest, url],
            capture_output=True, text=True, timeout=timeout + 30,
        )
    except subprocess.TimeoutExpired:
        return False, None, "download timed out"
    if result.returncode != 0:
        return False, None, f"curl exit code {result.returncode} ({result.stderr.strip()[:200]})"
    http_code = result.stdout.strip()
    if http_code != "200":
        return False, http_code, f"server responded HTTP {http_code}"
    if not os.path.exists(dest) or os.path.getsize(dest) == 0:
        return False, http_code, "downloaded file is empty"
    return True, http_code, None


def read_apk_metadata(apk_path):
    """Returns (package, version_name, app_name, error). Reads these straight
    out of the compiled AndroidManifest.xml/resources via androguard, rather
    than trusting hand-typed form fields that could drift from what was
    actually tested."""
    import logging
    logging.getLogger("androguard").setLevel(logging.ERROR)
    try:
        from androguard.core.apk import APK
        a = APK(apk_path)
        package = a.get_package()
        version_name = a.get_androidversion_name()
        app_name = a.get_app_name()
    except Exception as e:
        return None, None, None, f"couldn't read the APK's manifest ({e})"
    if not package:
        return None, None, None, "couldn't find a package name in the APK's manifest"
    return package, (version_name or "unknown"), (app_name or package), None


def _resolve_adaptive_icon_layer(apk_obj, axml_bytes):
    """Adaptive icons (<adaptive-icon>, API 26+) point at separate
    foreground/background layers instead of one bitmap. Those layers are
    usually VectorDrawable XML too (not worth rasterizing path data for
    this), but occasionally are plain PNG/WEBP — try the foreground layer,
    then background, and use whichever first resolves to an actual bitmap."""
    from androguard.core.axml import AXMLPrinter
    try:
        xml_text = AXMLPrinter(axml_bytes).get_xml().decode("utf-8", errors="ignore")
    except Exception:
        return None
    arsc = apk_obj.get_android_resources()
    for tag in ("foreground", "background"):
        m = re.search(rf'<{tag}[^>]*android:drawable="@([0-9A-Fa-f]+)"', xml_text)
        if not m:
            continue
        try:
            configs = arsc.get_resolved_res_configs(int(m.group(1), 16))
        except Exception:
            continue
        if not configs:
            continue
        layer_path = configs[0][1]
        if layer_path.lower().endswith((".png", ".webp")):
            try:
                return apk_obj.get_file(layer_path), (".png" if layer_path.lower().endswith(".png") else ".webp")
            except Exception:
                continue
    return None


def extract_icon(apk_path, out_path_no_ext):
    icon_bytes, icon_ext = None, None

    try:
        import logging
        logging.getLogger("androguard").setLevel(logging.ERROR)
        from androguard.core.apk import APK
        a = APK(apk_path)
        icon_res = a.get_app_icon()
        if icon_res and not icon_res.lower().endswith(".xml"):
            data = a.get_file(icon_res)
            if data:
                icon_bytes = data
                icon_ext = ".png" if icon_res.lower().endswith(".png") else ".webp"
        elif icon_res:
            resolved = _resolve_adaptive_icon_layer(a, a.get_file(icon_res))
            if resolved:
                icon_bytes, icon_ext = resolved
    except Exception:
        pass

    if icon_bytes is None:
        import zipfile
        try:
            with zipfile.ZipFile(apk_path) as z:
                candidates = []
                for name in z.namelist():
                    lower = name.lower()
                    if not (lower.startswith("res/mipmap") or lower.startswith("res/drawable")):
                        continue
                    base = lower.rsplit("/", 1)[-1]
                    if not (base.endswith(".png") or base.endswith(".webp")):
                        continue
                    if "ic_launcher" not in base and "icon" not in base:
                        continue
                    density_rank = 99
                    for i, d in enumerate(DENSITY_ORDER):
                        if d in lower:
                            density_rank = i
                            break
                    candidates.append((density_rank, -z.getinfo(name).file_size, name))
                if candidates:
                    candidates.sort()
                    best = candidates[0][2]
                    icon_bytes = z.read(best)
                    icon_ext = ".png" if best.lower().endswith(".png") else ".webp"
        except zipfile.BadZipFile:
            pass

    if icon_bytes is None:
        return None
    out_path = out_path_no_ext + icon_ext
    with open(out_path, "wb") as f:
        f.write(icon_bytes)
    return out_path


def check_play_store_category(package):
    url = f"https://play.google.com/store/apps/details?id={urllib.parse.quote(package)}&hl=en&gl=US"
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            status, html = resp.status, resp.read().decode("utf-8", errors="ignore")
    except urllib.error.HTTPError as e:
        return "NOT_FOUND" if e.code == 404 else "UNKNOWN"
    except Exception:
        return "UNKNOWN"
    if status != 200:
        return "UNKNOWN"
    if "/store/apps/category/GAME" in html:
        return "GAME"
    if "/store/apps/category/" in html:
        return "NOT_GAME"
    return "UNKNOWN"


def analyze_logs(launcher_log, compat_log, core_log):
    combined = "\n".join([launcher_log, compat_log, core_log])
    stalls, severe = [], 0
    for m in STALL_RE.finditer(combined):
        kind, ms = m.group(1), int(m.group(3))
        stalls.append(ms)
        if kind.startswith("STALL"):
            severe += 1

    first_error = None
    for line in combined.split("\n"):
        low = line.lower()
        if any(marker in low for marker in ERROR_MARKERS):
            first_error = line.strip()
            break

    frame_evidence = bool(FRAME_EVIDENCE_RE.search(combined)) or bool(stalls)

    if first_error and not frame_evidence:
        verdict = "fail"
        summary = "Fails to launch / crashes before ever rendering a frame."
    elif frame_evidence and severe == 0 and not first_error:
        verdict = "ok"
        summary = "Playable — no severe stalls or error markers detected."
    elif frame_evidence and (severe > 0 or first_error):
        worst = max(stalls) if stalls else 0
        bits = [f"{severe} severe stall(s), worst {worst}ms"]
        if first_error:
            bits.append("at least one error/crash marker logged")
        verdict = "issues"
        summary = "Runs, but with issues (" + "; ".join(bits) + ")."
    else:
        verdict = "unknown"
        summary = "Inconclusive — logs didn't match known success/failure patterns; needs manual review."

    return {
        "verdict": verdict,
        "summary": summary,
        "stall_count": len(stalls),
        "severe_count": severe,
        "max_stall_ms": max(stalls) if stalls else 0,
        "avg_stall_ms": round(sum(stalls) / len(stalls), 1) if stalls else 0,
        "first_error": first_error,
    }


def build_report_md(data, analysis, play_status, old_meta):
    lines = [
        f"# {data['game_name']} — {data['version_name']}", "",
        f"**Package:** `{data['package_name']}`  ",
        f"**Verdict:** {VERDICT_LABEL[analysis['verdict']]}  ",
        f"**Source:** {data['source_site']} — {data['apk_url']}  ",
        f"**Play Store category check:** {play_status}  ",
    ]
    if old_meta:
        lines.append(f"**Supersedes:** submission by @{old_meta.get('submitted_by')} on {old_meta.get('submitted_at')}  ")
    lines += ["", "## Analysis", "", analysis["summary"], "",
              f"- Frame stalls logged: **{analysis['stall_count']}** (severe: **{analysis['severe_count']}**)",
              f"- Worst stall: **{analysis['max_stall_ms']}ms**, average: **{analysis['avg_stall_ms']}ms**"]
    if analysis["first_error"]:
        lines.append(f"- First error/crash line found: `{analysis['first_error']}`")
    lines.append("")
    if data.get("notes"):
        lines += ["## Submitter notes", "", data["notes"], ""]
    lines += ["## Raw logs", "", "See `launcher_log.txt`, `compat_log.txt`, `core_log.txt` in this folder.", ""]
    return "\n".join(lines)


# -------------------------------------------------------------------- main --

def main():
    repo = os.environ["REPO_FULL"]
    issue_number = os.environ["ISSUE_NUMBER"]
    token = os.environ["GITHUB_TOKEN"]
    org_pat = os.environ.get("ORG_PAT", "").strip()
    issue_url = os.environ.get("ISSUE_URL", "")
    issue_user = os.environ.get("ISSUE_USER", "someone")
    body = os.environ.get("ISSUE_BODY", "")

    raw = parse_issue_body(body)
    data = {key: raw.get(label, "").strip() for label, key in LABEL_MAP.items()}

    def reject(reason):
        post_comment(repo, issue_number, token,
                      f"**This submission couldn't be processed automatically:**\n\n{reason}\n\n"
                      "Feel free to open a new report once that's fixed — closing this one now.")
        close_and_lock(repo, issue_number, token)
        sys.exit(0)

    missing = [k for k in REQUIRED_FIELDS if not data.get(k)]
    if missing:
        reject(f"Missing required field(s): {', '.join(missing)}.")

    apk_url = extract_apk_url(data["apk_url"])
    if not apk_url:
        reject("Couldn't find a `.apk` link in the upload field — either the drag-and-drop upload "
               "didn't attach properly, or the pasted link doesn't end in `.apk`. Split/`.xapk` "
               "packages aren't supported yet.")
    data["apk_url"] = apk_url

    workdir = tempfile.mkdtemp()
    apk_path = os.path.join(workdir, "game.apk")
    ok, http_code, err = download_apk(data["apk_url"], apk_path)
    if not ok:
        reject(f"Couldn't download the APK ({err or 'unknown error'}). "
               "Make sure the link is a direct, public, non-redirect-walled download.")

    with open(apk_path, "rb") as f:
        magic = f.read(2)
    if magic != b"PK":
        reject("The downloaded file isn't a valid APK/ZIP (bad magic bytes) — "
               "check the link points directly at the .apk, not an HTML landing page.")

    package, version_name, app_name, meta_err = read_apk_metadata(apk_path)
    if meta_err:
        reject(f"Couldn't read this APK ({meta_err}). Make sure it's a normal, unmodified .apk.")
    data["package_name"] = package
    data["version_name"] = version_name
    data["game_name"] = app_name

    icon_path = extract_icon(apk_path, os.path.join(workdir, "icon"))
    play_status = check_play_store_category(data["package_name"])
    if play_status == "NOT_GAME":
        reject(f"The Play Store listing for `{data['package_name']}` categorizes it as a non-game "
               "app, not a game — this project only tracks games. If that's wrong, open a fresh issue "
               "and mention it.")

    analysis = analyze_logs(data["launcher_log"], data["compat_log"], data["core_log"])

    if not org_pat:
        add_label(repo, issue_number, token, "needs-manual-review")
        post_comment(repo, issue_number, token,
            "Automated analysis finished but this repo isn't configured to publish results yet "
            "(the `ORG_PAT` secret isn't set) — a maintainer needs to add it. Leaving this issue "
            f"open with the analysis below so nothing is lost:\n\n"
            f"**Verdict:** {VERDICT_LABEL[analysis['verdict']]}\n{analysis['summary']}\n\n"
            f"Stalls: {analysis['stall_count']} (severe: {analysis['severe_count']}, "
            f"max {analysis['max_stall_ms']}ms, avg {analysis['avg_stall_ms']}ms)\n"
            + (f"First error/crash line: `{analysis['first_error']}`\n" if analysis["first_error"] else ""))
        sys.exit(0)

    reports_dir = os.path.join(workdir, "compat-reports")
    clone_url = f"https://{org_pat}@github.com/AndroidHorizon/compat-reports.git"
    subprocess.run(["git", "clone", "--depth", "1", clone_url, reports_dir], check=True, capture_output=True)

    pkg = safe_path_component(data["package_name"])
    ver = safe_path_component(data["version_name"])
    target = os.path.join(reports_dir, "reports", pkg, ver)

    old_meta = None
    if os.path.isdir(target):
        try:
            with open(os.path.join(target, "meta.json")) as f:
                old_meta = json.load(f)
        except Exception:
            old_meta = None
        shutil.rmtree(target)
    os.makedirs(target, exist_ok=True)

    with open(os.path.join(target, "launcher_log.txt"), "w") as f:
        f.write(data["launcher_log"])
    with open(os.path.join(target, "compat_log.txt"), "w") as f:
        f.write(data["compat_log"])
    with open(os.path.join(target, "core_log.txt"), "w") as f:
        f.write(data["core_log"])

    icon_rel = None
    if icon_path and os.path.exists(icon_path):
        icon_rel = "icon" + os.path.splitext(icon_path)[1]
        shutil.copy(icon_path, os.path.join(target, icon_rel))

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    meta = {
        "game_name": data["game_name"], "package_name": pkg, "version_name": ver,
        "apk_url": data["apk_url"], "source_site": data["source_site"],
        "submitted_by": issue_user, "submitted_at": now, "issue_url": issue_url,
        "notes": data.get("notes", ""), "play_store_category_check": play_status,
        "superseded_submission_by": old_meta.get("submitted_by") if old_meta else None,
    }
    with open(os.path.join(target, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    with open(os.path.join(target, "report.md"), "w") as f:
        f.write(build_report_md(data, analysis, play_status, old_meta))

    games_json_path = os.path.join(reports_dir, "data", "games.json")
    try:
        with open(games_json_path) as f:
            games = json.load(f)
    except Exception:
        games = []
    games = [g for g in games if g.get("package_name") != pkg]
    games.append({
        "game_name": data["game_name"], "package_name": pkg, "version_name": ver,
        "verdict": analysis["verdict"], "verdict_label": VERDICT_LABEL[analysis["verdict"]],
        "summary": analysis["summary"],
        "icon": f"reports/{pkg}/{ver}/{icon_rel}" if icon_rel else None,
        "source_site": data["source_site"], "submitted_by": issue_user, "submitted_at": now,
        "issue_url": issue_url, "report_path": f"reports/{pkg}/{ver}/report.md",
    })
    games.sort(key=lambda g: g["game_name"].lower())
    with open(games_json_path, "w") as f:
        json.dump(games, f, indent=2)

    subprocess.run(["git", "-C", reports_dir, "config", "user.name", "android-horizon-bot"], check=True)
    subprocess.run(["git", "-C", reports_dir, "config", "user.email",
                    "github-actions[bot]@users.noreply.github.com"], check=True)
    subprocess.run(["git", "-C", reports_dir, "add", "-A"], check=True)
    commit_msg = (f"{'Update' if old_meta else 'Add'} {data['game_name']} {ver} "
                  f"({VERDICT_LABEL[analysis['verdict']]})\n\nSubmitted by @{issue_user} — {issue_url}")
    subprocess.run(["git", "-C", reports_dir, "commit", "-m", commit_msg], check=True)
    subprocess.run(["git", "-C", reports_dir, "push", "origin", "main"], check=True)

    report_url = f"https://github.com/AndroidHorizon/compat-reports/blob/main/reports/{pkg}/{ver}/report.md"
    summary_comment = (
        f"**Automated analysis complete.**\n\n"
        f"**Verdict:** {VERDICT_LABEL[analysis['verdict']]}\n{analysis['summary']}\n\n"
        f"- Stalls logged: {analysis['stall_count']} (severe: {analysis['severe_count']}, "
        f"max {analysis['max_stall_ms']}ms, avg {analysis['avg_stall_ms']}ms)\n"
        + (f"- First error/crash line: `{analysis['first_error']}`\n" if analysis["first_error"] else "")
        + f"- Play Store category check: {play_status}\n"
        + (f"- This overwrites a previous submission for the same version (by @{old_meta.get('submitted_by')})\n"
           if old_meta else "")
        + f"\nFull report + logs: {report_url}\n"
          "Compatibility page: https://androidhorizon.github.io/website/compatibility.html\n\n"
          "Thanks for testing! Closing this issue now — it's a data submission, not a discussion thread."
    )
    post_comment(repo, issue_number, token, summary_comment)
    close_and_lock(repo, issue_number, token)


if __name__ == "__main__":
    main()
