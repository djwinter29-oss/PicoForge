"""Check the shared dashboard through real Python and .NET hosting without hardware requests."""

from contextlib import contextmanager
from pathlib import Path
from threading import Thread
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from playwright.sync_api import sync_playwright
from werkzeug.serving import make_server

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "host/python/src"))
from picoforge.dashboard.boards import DashboardBoards
from picoforge.dashboard.server import create_app

CHECKS = Path(__file__).with_name("checks.mjs").read_text()
MOCK = re.search(r"const mock = `<script>\n(.*?)</script>", CHECKS, re.DOTALL).group(1)
PERFORMANCE = Path(__file__).with_name("console-performance.mjs").read_text()


@contextmanager
def python_host():
    """Serve actual Python templates, asset routes, and response headers."""
    boards = DashboardBoards(discover=lambda: [])
    server = make_server("127.0.0.1", 0, create_app(boards, ROOT / "host/web/src"), threaded=True)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
        boards.close()


@contextmanager
def dotnet_host():
    """Run the built .NET host and stop only the process owned by this test."""
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    origin = f"http://127.0.0.1:{port}"
    dll = ROOT / "host/dotnet/PicoForge/bin/Debug/net10.0/PicoForge.dll"
    with tempfile.TemporaryFile(mode="w+") as output:
        process = subprocess.Popen(["dotnet", str(dll), "--port", str(port)], stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 20
            while True:
                if process.poll() is not None:
                    output.seek(0)
                    raise RuntimeError(f".NET host exited during startup:\n{output.read()}")
                try:
                    with urllib.request.urlopen(origin, timeout=1) as response:
                        if response.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    if time.monotonic() >= deadline:
                        output.seek(0)
                        raise RuntimeError(f".NET host did not become ready:\n{output.read()}")
                    time.sleep(0.1)
            yield origin
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main() -> None:
    """Fail on incomplete checks, assertion failures, page errors, or missing hosting assets."""
    subprocess.run(["dotnet", "build", str(ROOT / "host/dotnet/PicoForge/PicoForge.csproj"), "--nologo"],
                   check=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            for name, host in (("Python", python_host), (".NET", dotnet_host)):
                with host() as origin, browser.new_context(viewport={"width": 360, "height": 800}) as context:
                    page = context.new_page()
                    errors: list[str] = []
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    page.add_init_script(MOCK)
                    page.route("**/__browser_checks__.mjs", lambda route: route.fulfill(
                        status=200, content_type="text/javascript", body=CHECKS))
                    response = page.goto(origin)
                    assert response.status == 200
                    assert "script-src 'self'" in response.headers["content-security-policy"]
                    assert name in page.locator(".brand-copy").inner_text()
                    reports = page.evaluate("""async () => {
                        globalThis.sharedBrowserHosted = true;
                        await import('/__browser_checks__.mjs');
                        return globalThis.sharedBrowserResults;
                    }""")
                    for report in reports:
                        print(f"{name}: {'FAIL' if report['error'] else 'PASS'} {report['name']}"
                              + (f": {report['error']}" if report["error"] else ""))
                    assert len(reports) == 4 and not any(report["error"] for report in reports) and not errors, errors
                    page.set_viewport_size({"width": 1000, "height": 800})
                    page.goto(f"{origin}/console")
                    measurement = page.evaluate("async () => {\n" + PERFORMANCE + "\nreturn await measureConsole(); }")
                    print(f"{name} console rendering: {json.dumps(measurement)}")
                    assert measurement["renders"]["median"] == 1 and measurement["renders"]["p95"] == 1, measurement
                    assert not errors, errors
        finally:
            browser.close()


if __name__ == "__main__":
    main()
