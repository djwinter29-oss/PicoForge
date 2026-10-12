# PicoForge WebHID

Serve the WebHID directory directly so its `index.html` is the site root and the browser gets a secure localhost origin:

```sh
python3 -m http.server 8000 --directory host/webhid
```

Open `http://localhost:8000/` in a WebHID-capable browser, connect the board, and grant access to its vendor HID interface. When MSC is enabled or disabled, USB briefly re-enumerates; the dashboard automatically reopens the same authorized HID device by board serial. Safely eject the host drive before disabling MSC so PicoForge can return SD-card ownership to firmware. The page owns its HTML, CSS, view logic, and favicon. It shares only the HID wire protocol with the Python and .NET hosts. USB serial support in a browser is not used; CDC remains available to terminal applications.