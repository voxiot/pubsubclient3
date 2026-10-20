"""Compile-only checks for configurable limits; no firmware or executable is built."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++', help='C++ compiler executable')
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    cases = [('defaults', [], True, '')]
    for macro, values, diagnostic in [
        ('MQTT_RX_QOS2_MAX_INFLIGHT', [1, 4, 65535, 0, -1, 65536],
         'MQTT_RX_QOS2_MAX_INFLIGHT must be between 1 and 65535'),
        ('MQTT_SOCKET_TIMEOUT', [1, 65535, 0, -1, 65536],
         'MQTT_SOCKET_TIMEOUT must be between 1 and 65535 seconds'),
    ]:
        for value in values:
            cases.append((f'{macro}={value}', [f'-D{macro}={value}'],
                          1 <= value <= 65535, diagnostic))
    with tempfile.TemporaryDirectory(prefix='pubsub-config-') as temporary:
        source = Path(temporary) / 'config.cpp'
        source.write_text('#include "PubSubClient.h"\n', encoding='utf-8')
        for name, defines, should_compile, diagnostic in cases:
            result = subprocess.run(
                [args.compiler, '-std=c++11', '-fsyntax-only',
                 '-I' + str(tests / 'src/lib'), '-I' + str(tests.parent / 'src'),
                 *defines, str(source)], capture_output=True, text=True)
            output = result.stdout + result.stderr
            if (result.returncode == 0) != should_compile or (
                    not should_compile and diagnostic not in output):
                raise SystemExit(f'FAIL {name}\n{output}')
            print(f'PASS {name}: ' + ('accepted' if should_compile else 'rejected by static_assert'))
    print(f'{len(cases)}/{len(cases)} configuration checks passed')


if __name__ == '__main__':
    main()
