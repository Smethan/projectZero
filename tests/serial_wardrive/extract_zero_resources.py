"""Extract the production Zero admission/ownership functions for fault injection."""
from pathlib import Path
import sys

source = Path('../../ESP32C5/main/main.c').read_text()
start = source.index('static void *zero_job_allocations[16];')
end = source.index('\n#endif', start)
Path(sys.argv[1], 'zero_resources_under_test.inc').write_text(source[start:end]+'\n')
start = source.index('static void zero_job_restore_sta_after_partial_portal(void) {')
end = source.index('\nstatic int zero_job_start_operation(', start)
Path(sys.argv[1], 'portal_cleanup_under_test.inc').write_text(source[start:end]+'\n')
