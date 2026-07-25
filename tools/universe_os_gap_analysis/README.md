# Universe OS Gap Analysis Tool

This package is an independent, read-only repository assessment tool. It does not import Nebula compiler, runtime, kernel, driver, or product modules.

Run the configuration-only skeleton from the repository root:

```bash
python3 -m tools.universe_os_gap_analysis \
  --repo-root . \
  --output-dir assessments/universe-os \
  --dry-run
```

Network access and external command execution are disabled by default. The tool may read the selected repository and, once the pipeline is implemented, write only to the explicitly supplied assessment output directory.
