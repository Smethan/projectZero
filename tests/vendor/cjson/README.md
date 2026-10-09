These MIT-licensed cJSON files come from the `espressif/cjson` dependency resolved
by the pinned ESP-IDF 6.0.1 build. They let the native job harness execute the
production JSON parser and encoded requests instead of the malformed-only shim.
The firmware build still resolves its normal ESP-IDF managed dependency.
