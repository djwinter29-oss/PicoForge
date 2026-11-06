# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Check the shared dashboard through real Python and .NET hosting without hardware requests."""

from contextlib import contextmanager
from pathlib import Path
from threading import Thread
import argparse
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from playwright.sync_api import Page, expect, sync_playwright
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


def check_native_drafts(page: Page, origin: str, host_name: str) -> None:
    """Use native control events to verify Configure is never an edit and only explicit disables erase credentials."""
    for radio, bit in (("wifi", 1), ("ble", 2)):
        for enabled in (True, False):
            page.goto(origin)
            page.wait_for_function("!document.querySelector('#settings-ble').disabled")
            page.locator(f"#settings-{radio}").focus()
            page.evaluate("""({radio, bit, enabled}) => {
                const state = window.sharedBrowserState;
                state.connected.board_configuration.radios = enabled ? bit : 0;
                Object.assign(state.connected.wireless, {
                    ssid: 'saved', ssid_hex: '7361766564', ble_key_configured: true,
                    wifi_enabled: radio === 'wifi' && enabled, ble_enabled: radio === 'ble' && enabled
                });
                document.dispatchEvent(new Event('visibilitychange'));
            }""", {"radio": radio, "bit": bit, "enabled": enabled})
            value = "1" if enabled else "0"
            page.wait_for_function(
                "({radio, value}) => document.querySelector('#settings-' + radio).value === value",
                arg={"radio": radio, "value": value})
            configure = page.locator(f"#{radio}-configure")
            configure.focus()
            configure.press("Enter")
            assert page.locator(f"#{radio}-dialog").evaluate("(dialog) => dialog.open")
            page.keyboard.press("Escape")
            page.wait_for_function("(radio) => !document.querySelector('#' + radio + '-dialog').open", arg=radio)
            assert configure.evaluate("(button) => button === document.activeElement")
            assert page.locator(f"#settings-{radio}").input_value() == value
            assert page.locator("#settings-save").is_disabled(), "Configure manufactured an edit"
            page.evaluate("document.dispatchEvent(new Event('visibilitychange'))")
            page.wait_for_timeout(30)
            assert page.locator("#settings-save").is_disabled(), "Repeated status manufactured an edit"
            if enabled:
                page.select_option(f"#settings-{radio}", "0")
                page.locator("#settings-save").click()
                page.locator("#confirm-configuration").click()
                page.wait_for_function("window.sharedBrowserState.wirelessRequests.length === 1")
                request = page.evaluate("window.sharedBrowserState.wirelessRequests[0]")
                if radio == "wifi":
                    assert request["ssid"] == request["password"] == "" and "ble_key" not in request
                else:
                    assert request["ble_key"] == "" and "ssid" not in request and "password" not in request
            else:
                page.select_option("#settings-clock", "250")
                page.evaluate("""(radio) => {
                    document.querySelector('#settings-clock').value = '0';
                    document.querySelector('#settings-storage').value = '1';
                    document.querySelector('#settings-' + radio).value = '1';
                }""", radio)
                page.locator("#settings-save").click()
                page.locator("#confirm-configuration").click()
                page.wait_for_function("window.sharedBrowserState.settingsRequests.length === 1")
                assert page.evaluate("window.sharedBrowserState.settingsRequests[0]") == {
                    "system_clock_mhz": 250, "sdcard_profile": 0, "radios": 0}
                assert page.evaluate("window.sharedBrowserState.wirelessRequests.length") == 0
            print(f"{host_name}: PASS native {radio} Configure after remote {'enable' if enabled else 'disable'}")


def check_native_transitions(page: Page, origin: str, host_name: str) -> None:
    """Verify confirmation identity, dependent cleanup and local conflict recovery with native events."""
    page.goto(origin)
    page.wait_for_function("!document.querySelector('#settings-clock').disabled")
    page.select_option("#settings-clock", "250")
    page.locator("#settings-save").click()
    assert "SD Card: Standard" in page.locator("#configuration-dialog-profile").inner_text()
    page.evaluate("""() => {
        window.sharedBrowserState.connected.board_configuration.sdcard_profile = 1;
        document.dispatchEvent(new Event('visibilitychange'));
    }""")
    page.wait_for_function("document.querySelector('#settings-storage').value === '1'")
    page.locator("#confirm-configuration").click()
    assert page.evaluate("window.sharedBrowserState.settingsRequests.length") == 0
    assert "confirm again" in page.locator("#settings-note").inner_text()
    page.locator("#settings-save").click()
    assert "High-Speed" in page.locator("#configuration-dialog-profile").inner_text()
    page.locator("#confirm-configuration").click()
    page.wait_for_function("window.sharedBrowserState.settingsRequests.length === 1")
    assert page.evaluate("window.sharedBrowserState.settingsRequests[0]") == {
        "system_clock_mhz": 250, "sdcard_profile": 1, "radios": 0}
    print(f"{host_name}: PASS native renewed confirmation after a remote proposal change")

    for conflicting in (False, True):
        page.goto(origin)
        page.wait_for_function("!document.querySelector('#wifi-configure').disabled")
        page.select_option("#settings-clock", "250")
        page.select_option("#settings-storage", "1")
        page.locator("#wifi-configure").click()
        page.locator("#wifi-ssid").fill("temporary-network")
        page.locator("#wifi-connect").click()
        page.wait_for_function("!document.querySelector('#wifi-dialog').open")
        if conflicting:
            page.locator("#ble-configure").click()
            page.locator("#ble-generate").click()
            page.locator("#ble-cancel").click()
            page.evaluate("""() => {
                const state = window.sharedBrowserState;
                state.connected.wireless.tcp_cli_enabled = false;
                state.savedWireless.tcp_cli_enabled = false;
                document.dispatchEvent(new Event('visibilitychange'));
            }""")
            page.locator("#wireless-discard").wait_for(state="visible")
            assert page.locator("#settings-save").is_disabled()
            page.locator("#wireless-discard").click()
        else:
            page.locator("#wifi-configure").click()
            page.locator("#wifi-cancel").click()
        page.wait_for_function("document.querySelector('#wifi-cancel').textContent === 'Retry clearing test'")
        assert page.locator("#wifi-dialog").evaluate("(dialog) => dialog.open")
        assert page.locator("#settings-save").is_disabled()
        if conflicting:
            assert page.locator("#ble-secret").input_value(), "failed cleanup lost the replacement key"
        page.locator("#wifi-cancel").click()
        page.wait_for_function("!document.querySelector('#wifi-dialog').open")
        if conflicting:
            page.locator("#wireless-discard").click()
            page.wait_for_function("document.querySelector('#wireless-discard').hidden")
            page.wait_for_function("document.querySelector('#settings-tcp-cli').value === '0'")
            assert page.locator("#ble-secret").input_value() == ""
        for _ in range(3):
            page.evaluate("document.dispatchEvent(new Event('visibilitychange'))")
            page.wait_for_timeout(30)
        assert page.locator("#settings-wifi").input_value() == "0"
        assert page.locator("#settings-ble").input_value() == "0"
        assert page.locator("#settings-clock").input_value() == "250"
        assert page.locator("#settings-storage").input_value() == "1"
        page.locator("#settings-save").click()
        page.locator("#confirm-configuration").click()
        page.wait_for_function("window.sharedBrowserState.settingsRequests.length === 1")
        assert page.evaluate("window.sharedBrowserState.settingsRequests[0]") == {
            "system_clock_mhz": 250, "sdcard_profile": 1, "radios": 0}
        assert page.evaluate("window.sharedBrowserState.wirelessRequests.length") == 0
        print(f"{host_name}: PASS native {'local conflict discard' if conflicting else 'dependent Wi-Fi cleanup'}"
              " preserves unrelated drafts")


def check_accessibility_smoke(page: Page, origin: str, host_name: str) -> None:
    """Traverse native focus order, check visible focus, and exercise confirmation and clipboard refusal."""
    page.add_init_script("""
        window.confirmationCloseListeners = 0;
        const add = EventTarget.prototype.addEventListener;
        EventTarget.prototype.addEventListener = function(type, listener, options) {
            if (this.id === 'configuration-dialog' && type === 'close') window.confirmationCloseListeners++;
            return add.call(this, type, listener, options);
        };
    """)
    page.goto(origin)
    expect(page.locator("#settings-clock")).to_be_enabled()

    def check_focus() -> None:
        assert page.evaluate("""() => {
            const element = document.activeElement;
            if (element === document.body) return true;
            const rect = element.getBoundingClientRect(), style = getComputedStyle(element);
            const hit = document.elementFromPoint(rect.x + rect.width / 2, rect.y + rect.height / 2);
            return rect.width > 0 && rect.height > 0 && rect.top >= 0 && rect.left >= 0
                && rect.bottom <= innerHeight && rect.right <= innerWidth
                && style.outlineStyle !== 'none' && parseFloat(style.outlineWidth) > 0
                && (hit === element || element.contains(hit));
        }"""), "Keyboard focus must have a visible outline and remain in-view and unobscured"

    def tab_to(selector: str, reverse: bool = False) -> None:
        for _ in range(40):
            page.keyboard.press("Shift+Tab" if reverse else "Tab")
            check_focus()
            if page.locator(selector).evaluate("(element) => element === document.activeElement"):
                return
        raise AssertionError(f"Keyboard traversal did not reach {selector}")

    tab_to("#settings-clock")
    page.keyboard.press("ArrowDown")
    tab_to("#settings-save", reverse=True)
    page.keyboard.press("Enter")
    dialog = page.get_by_role("dialog", name="Save and reboot?")
    expect(dialog).to_be_visible()
    assert dialog.get_by_role("button", name="Cancel", exact=True).evaluate(
        "(button) => button === document.activeElement")
    check_focus()
    tab_to("#confirm-configuration")
    tab_to("#cancel-configuration", reverse=True)
    for _ in range(50):
        page.keyboard.press("Tab")
    assert page.evaluate("window.confirmationCloseListeners") == 1
    page.keyboard.press("Escape")
    expect(dialog).not_to_be_visible()
    assert page.locator("#settings-save").evaluate("(button) => button === document.activeElement")
    check_focus()
    assert page.evaluate("window.sharedBrowserState.settingsRequests.length") == 0
    print(f"{host_name}: PASS native Tab/Shift+Tab journey, visible focus, confirmation and bounded close listeners")

    page.set_viewport_size({"width": 1280, "height": 900})
    page.evaluate("document.documentElement.style.zoom = '2'")
    assert page.evaluate("document.documentElement.scrollWidth <= document.documentElement.clientWidth")
    page.evaluate("document.documentElement.style.zoom = ''")
    page.set_viewport_size({"width": 320, "height": 800})
    assert page.evaluate("document.documentElement.scrollWidth <= document.documentElement.clientWidth")
    tab_to("#ble-configure")
    page.keyboard.press("Enter")
    tab_to("#ble-generate")
    page.keyboard.press("Enter")
    page.evaluate("""() => Object.defineProperty(navigator, 'clipboard', {configurable: true,
        value: {writeText: async () => {throw new DOMException('Clipboard denied', 'NotAllowedError');}}})""")
    tab_to("#ble-copy")
    page.keyboard.press("Enter")
    expect(page.locator("#ble-status")).to_contain_text("Could not copy key")
    assert "Key copied" not in page.locator("#ble-status").inner_text()
    assert page.get_by_role("dialog", name="Bluetooth control key").is_visible()
    assert page.evaluate("document.querySelector('#ble-dialog').scrollWidth <= "
                         "document.querySelector('#ble-dialog').clientWidth")
    page.keyboard.press("Escape")
    assert page.locator("#ble-configure").evaluate("(button) => button === document.activeElement")
    check_focus()
    print(f"{host_name}: PASS 200-percent zoom, 320-pixel reflow and clipboard refusal feedback")


def check_partial_diagnostics(page: Page, origin: str, host_name: str) -> None:
    """Prove optional diagnostic failures do not invent observed states or discard the settings draft."""
    page.goto(origin)
    expect(page.locator("#settings-clock")).to_be_enabled()
    page.locator("#settings-clock").select_option("250")
    page.evaluate("""() => {
        const state = window.sharedBrowserState.connected;
        state.runtime.cdc_open = 'invalid';
        state.hardware.product_name = {};
        delete state.hardware.board_model;
        state.board_configuration.monitor_uart_index = 'invalid';
        state.board_configuration.monitor_uart_tx_gpio = -1;
        state.board_configuration.monitor_uart_rx_gpio = 255;
        state.board_configuration.storage_gpios = [0, 255];
        state.board_configuration.reset_cause = 'invalid';
    }""")
    expect(page.locator("#status-message")).to_contain_text("runtime.cdc_open")
    expect(page.locator("#cdc-state")).to_have_text("--")
    expect(page.locator("#product-name")).to_have_text("--")
    expect(page.locator("#board-model")).to_have_text("-- (RP2350)")
    expect(page.locator("#board-uart-title")).to_have_text("Monitor UART")
    expect(page.locator("#board-uart td").nth(0)).to_have_text("--")
    expect(page.locator("#board-uart td").nth(1)).to_have_text("Unassigned")
    expect(page.locator("#board-storage-pins td").nth(0)).to_have_text("GP0")
    expect(page.locator("#board-storage-pins td").nth(1)).to_have_text("Unassigned")
    expect(page.locator("#board-storage-pins td").nth(2)).to_have_text("--")
    expect(page.locator("#board-reset-cause")).to_have_text("--")
    expect(page.locator("#settings-clock")).to_have_value("250")
    expect(page.locator("#led-control")).to_be_enabled()
    expect(page.locator("#settings-save")).to_be_enabled()
    page.evaluate("""() => {
        const state = window.sharedBrowserState.connected;
        state.runtime.cdc_open = false;
        state.board_configuration.monitor_uart_index = 0;
        state.board_configuration.monitor_uart_tx_gpio = 0;
        state.board_configuration.reset_cause = 0;
    }""")
    expect(page.locator("#cdc-state")).to_have_text("Closed")
    expect(page.locator("#board-uart-title")).to_have_text("Monitor UART0")
    expect(page.locator("#board-uart td").nth(0)).to_have_text("GP0")
    expect(page.locator("#board-reset-cause")).to_have_text("Unknown")
    page.evaluate("() => { window.sharedBrowserState.connected.runtime.cdc_open = true; }")
    expect(page.locator("#cdc-state")).to_have_text("Open")
    print(f"{host_name}: PASS unknown/false/sentinel diagnostics preserve valid commands and settings drafts")


def main() -> None:
    """Fail on incomplete checks, assertion failures, page errors, or missing hosting assets."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", nargs="+", choices=("chromium", "firefox", "webkit"), default=["chromium"])
    args = parser.parse_args()
    subprocess.run(["dotnet", "build", str(ROOT / "host/dotnet/PicoForge/PicoForge.csproj"), "--nologo"],
                   check=True)
    with sync_playwright() as playwright:
        for engine in args.browser:
            browser = getattr(playwright, engine).launch()
            try:
                for name, host in (("Python", python_host), (".NET", dotnet_host)):
                    label = f"{engine}/{name}"
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
                            print(f"{label}: {'FAIL' if report['error'] else 'PASS'} {report['name']}"
                                  + (f": {report['error']}" if report["error"] else ""))
                        assert len(reports) == 17 and not any(report["error"] for report in reports) and not errors, errors
                        check_native_drafts(page, origin, label)
                        check_native_transitions(page, origin, label)
                        check_accessibility_smoke(page, origin, label)
                        check_partial_diagnostics(page, origin, label)
                        page.set_viewport_size({"width": 1000, "height": 800})
                        page.goto(f"{origin}/console")
                        measurement = page.evaluate("async () => {\n" + PERFORMANCE + "\nreturn await measureConsole(); }")
                        print(f"{label} console rendering: {json.dumps(measurement)}")
                        assert measurement["renders"]["median"] == 1 and measurement["renders"]["p95"] == 1, measurement
                        assert measurement["rowChanges"]["p95"] <= 24, measurement
                        assert not errors, errors
            finally:
                browser.close()


if __name__ == "__main__":
    main()
