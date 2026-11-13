# AMD FSR SDK headers

Public API headers from AMD FSR SDK v2.4.0, revision
`7539ae8b6568ad29ed78a753404351daf3b5cca7`:
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX

Only trailing whitespace is normalized; the declarations and MIT notices are unchanged.
`LICENSE-SDK.md` contains AMD's complete
SDK licence, including the binary runtime redistribution notice and exceptions.
The runtime DLLs are optional, are not committed, and must stay unmodified.

From the repository root, stage the signed Vulkan DLLs beside a built executable:

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_fsr3_runtime.ps1 -Destination build/fsr3-preview
```

The script pins the release revision and DLL hashes, verifies AMD's Authenticode
signature, and copies the SDK licence beside them. The application queries
supported providers and explicitly selects FSR 3.1; it does not silently select
an ML upscaler or label another technique FSR 4.
