"""Run the Direct-only browser checks against an ephemeral loopback static server."""

from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import sync_playwright


def main() -> None:
    """Fail on browser errors, incomplete checks, or any reported assertion failure."""
    root = Path(__file__).resolve().parents[2]
    with ThreadingHTTPServer(("127.0.0.1", 0), partial(SimpleHTTPRequestHandler, directory=str(root))) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch()
                try:
                    page = browser.new_page()
                    errors: list[str] = []
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    page.goto(f"http://127.0.0.1:{server.server_port}/tests/browser/index.html")
                    page.wait_for_function("document.querySelector('#results').dataset.complete === 'true'")
                    results = page.locator("#results li").all_text_contents()
                    print("\n".join(results))
                    print(f"Console rendering: {page.locator('#results').get_attribute('data-console-median-ms')}ms "
                          f"median, {page.locator('#results').get_attribute('data-console-p95-ms')}ms p95")
                    failures = page.locator('#results li[data-result="fail"]').count()
                    if len(results) != 5 or failures or errors:
                        raise AssertionError(f"Expected 5 checks, got {len(results)}; "
                                             f"failed assertions: {failures}; page errors: {errors}")
                finally:
                    browser.close()
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
