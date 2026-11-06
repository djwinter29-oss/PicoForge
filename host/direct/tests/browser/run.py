"""Run the Direct-only browser checks against an ephemeral loopback static server."""

from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import Browser, expect, sync_playwright


def check_history(browser: Browser, origin: str) -> None:
    """Navigate real cached pages; require reload without resuming drafts or terminal output."""
    for target in ("index.html", "console.html?ipv4=192.168.1.9"):
        with browser.new_context() as context:
            page = context.new_page()
            bridge_requests: list[str] = []
            page.on("request", lambda request: bridge_requests.append(request.url)
                    if "/api/direct-terminal/" in request.url else None)
            page.add_init_script("""
                // Registering WebHID listeners makes Chromium pages ineligible for its history cache.
                Object.defineProperty(navigator, 'hid', {configurable: true, value: undefined});
                if (location.pathname.endsWith('/index.html') || location.pathname.endsWith('/console.html')) {
                    sessionStorage.setItem('test-history-loads',
                        String(Number(sessionStorage.getItem('test-history-loads') || 0) + 1));
                    addEventListener('pageshow', event => {
                        if (event.persisted) sessionStorage.setItem('test-history-restored', 'yes');
                    });
                }
            """)
            page.goto(f"{origin}/web/{target}")
            dashboard = target == "index.html"
            page.wait_for_selector("#settings-clock" if dashboard else "#terminal")
            page.evaluate("""dashboard => {
                sessionStorage.setItem('picoforge.direct.pending-operation', JSON.stringify({
                    version: 2, kind: 'control', uid: '0102030405060708',
                    request: {transaction_id: 1, opcode: 1}
                }));
                if (dashboard) document.querySelector('#direct-ble-control-key').value = 'old draft';
                else document.querySelector('#terminal').textContent = 'old terminal output';
            }""", dashboard)
            page.goto(f"{origin}/")
            page.go_back()
            if dashboard:
                expect(page.locator("#direct-ble-control-key")).to_have_value("")
                if page.locator("#connection").get_attribute("data-state") == "connected":
                    raise AssertionError("History restoration resumed the board connection")
            else:
                expect(page.locator("#terminal")).not_to_have_text("old terminal output")
                if page.locator("#connect").text_content() != "Connect":
                    raise AssertionError("History restoration resumed the console session")
            restored = page.evaluate("""() => sessionStorage.getItem('test-history-restored') === 'yes'
                && Number(sessionStorage.getItem('test-history-loads')) >= 2""")
            if not restored:
                reasons = page.evaluate("() => performance.getEntriesByType('navigation')[0]?.notRestoredReasons")
                raise AssertionError(f"Navigation did not exercise history-cache restoration followed by reload: "
                                     f"{reasons}")
            retained = page.evaluate("""() => {
                const record = JSON.parse(sessionStorage.getItem('picoforge.direct.pending-operation'));
                return record?.uid === '0102030405060708' && record.version === 2
                    && record.request.transaction_id === 1 && record.request.opcode === 1;
            }""")
            if not retained:
                raise AssertionError("History restoration lost the recovery journal")
            if bridge_requests:
                raise AssertionError(f"History navigation unexpectedly opened a terminal: {bridge_requests}")
            print(f"PASS {target.split('?')[0]} history-cache restoration reloads without replay")


def main() -> None:
    """Fail on browser errors, incomplete checks, or any reported assertion failure."""
    root = Path(__file__).resolve().parents[2]
    with ThreadingHTTPServer(("127.0.0.1", 0), partial(SimpleHTTPRequestHandler, directory=str(root))) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(channel="chromium",
                                                     ignore_default_args=["--disable-back-forward-cache"])
                try:
                    page = browser.new_page()
                    errors: list[str] = []
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    origin = f"http://127.0.0.1:{server.server_port}"
                    page.goto(f"{origin}/tests/browser/index.html")
                    page.wait_for_function("document.querySelector('#results').dataset.complete === 'true'")
                    results = page.locator("#results li").all_text_contents()
                    print("\n".join(results))
                    print(f"Console rendering: {page.locator('#results').get_attribute('data-console-median-ms')}ms "
                          f"median, {page.locator('#results').get_attribute('data-console-p95-ms')}ms p95")
                    for label, key in (("Dashboard telemetry burst", "dashboard"), ("Console streaming", "streaming")):
                        print(f"{label}: {page.locator('#results').get_attribute(f'data-{key}-median-ms')}ms median, "
                              f"{page.locator('#results').get_attribute(f'data-{key}-p95-ms')}ms p95")
                    for phase in ("parse", "text", "dom", "layout"):
                        print(f"Streaming {phase}: "
                              f"{page.locator('#results').get_attribute(f'data-streaming-{phase}-median-ms')}ms median, "
                              f"{page.locator('#results').get_attribute(f'data-streaming-{phase}-p95-ms')}ms p95")
                    failures = page.locator('#results li[data-result="fail"]').count()
                    if len(results) != 9 or failures or errors:
                        raise AssertionError(f"Expected 9 checks, got {len(results)}; "
                                             f"failed assertions: {failures}; page errors: {errors}")
                    check_history(browser, origin)
                finally:
                    browser.close()
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
