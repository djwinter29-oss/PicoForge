# PicoForge WebHID

Serve the repository's `host` directory so the browser gets a secure localhost origin:

```sh
python3 -m http.server 8000 --directory host
```

Open `http://localhost:8000/webhid/` in a WebHID-capable browser, connect the board, and grant access to its vendor HID interface. The page reads the same feature and input reports as the Python and .NET hosts. USB serial support in a browser is not used; CDC remains available to terminal applications.