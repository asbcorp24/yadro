Bootstrap 5.3.3 runtime assets are stored in this directory.

CMake downloads the pinned files on first configure:
- bootstrap.min.css
- bootstrap.bundle.min.js

After that the web UI uses only local files under data/static/vendor/bootstrap and does not require Internet access at runtime.
