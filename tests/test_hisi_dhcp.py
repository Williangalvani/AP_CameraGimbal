#!/usr/bin/env python3
"""Run the Hi3516DV500 udhcpc event script with fake ifconfig and route."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hisi-dhcp-') as directory:
    root = Path(directory)
    commands = root / 'commands'
    commands.mkdir()
    calls = root / 'calls'
    routes = root / 'routes'
    saved = root / 'udhcpc.gateway'
    # Default routes only, one gateway per line, in the order the kernel lists them.
    (commands / 'ifconfig').write_text('''#!/bin/sh
echo "ifconfig $*" >>"$TEST_ROOT/calls"
''')
    (commands / 'route').write_text('''#!/bin/sh
routes=$TEST_ROOT/routes
case $1 in
-n)
    echo 'Kernel IP routing table'
    echo 'Destination     Gateway         Genmask         Flags Metric Ref    Use Iface'
    while read -r gateway; do
        echo "0.0.0.0         $gateway     0.0.0.0         UG    0      0        0 eth0"
    done <"$routes"
    ;;
del)
    echo "route $*" >>"$TEST_ROOT/calls"
    [ -s "$routes" ] || exit 1
    sed 1d "$routes" >"$routes.new" && mv "$routes.new" "$routes"
    ;;
add)
    echo "route $*" >>"$TEST_ROOT/calls"
    echo "$4" >>"$routes"
    ;;
esac
''')
    for path in commands.iterdir():
        path.chmod(0o755)

    def run(event, **lease):
        calls.write_text('')
        env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
                   TEST_ROOT=str(root), DHCP_SAVED_GATEWAY=str(saved), interface='eth0', **lease)
        result = subprocess.run(['sh', str(repo / 'packaging/hisi/udhcpc.sh'), event],
                                env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (event, result.returncode, result.stderr)
        made = calls.read_text().splitlines()
        assert not any(call.startswith('ifconfig eth0 ') for call in made), made
        return made

    def defaults():
        return routes.read_text().split()

    lease = dict(ip='192.168.15.5', subnet='255.255.255.0', router='192.168.15.1 192.168.15.2')

    # The client deconfigures first; with no lease yet nothing else changes.
    routes.write_text('192.168.2.1\n')
    assert run('deconfig') == ['ifconfig eth0:1 down']
    assert defaults() == ['192.168.2.1'] and not saved.exists()

    # A lease adds the alias and moves the default route to its first router,
    # remembering the fixed gateway.
    assert run('bound', **lease) == [
        'ifconfig eth0:1 192.168.15.5 netmask 255.255.255.0 up',
        'route del default', 'route del default',
        'route add default gw 192.168.15.1 dev eth0']
    assert defaults() == ['192.168.15.1'] and saved.read_text() == '192.168.2.1\n'

    # A renewal on the same router leaves the route alone and keeps the saved one.
    assert run('renew', **lease) == ['ifconfig eth0:1 192.168.15.5 netmask 255.255.255.0 up']
    assert defaults() == ['192.168.15.1'] and saved.read_text() == '192.168.2.1\n'

    # A new lease with another router moves the route again.
    assert run('bound', ip='10.0.0.7', subnet='255.0.0.0', router='10.0.0.1')[-1] == \
        'route add default gw 10.0.0.1 dev eth0'
    assert defaults() == ['10.0.0.1'] and saved.read_text() == '192.168.2.1\n'

    # When the lease ends the alias goes and the fixed gateway comes back.
    assert run('deconfig') == [
        'ifconfig eth0:1 down', 'route del default', 'route del default',
        'route add default gw 192.168.2.1 dev eth0']
    assert defaults() == ['192.168.2.1'] and not saved.exists()

    # A lease without a router leaves the routes alone.
    assert run('bound', ip='192.168.15.5', subnet='255.255.255.0') == \
        ['ifconfig eth0:1 192.168.15.5 netmask 255.255.255.0 up']
    assert defaults() == ['192.168.2.1'] and not saved.exists()

    # Without a fixed gateway the lease's route is removed again at the end.
    routes.write_text('')
    run('bound', **lease)
    assert defaults() == ['192.168.15.1'] and saved.read_text() == ''
    assert run('deconfig') == ['ifconfig eth0:1 down', 'route del default', 'route del default']
    assert defaults() == [] and not saved.exists()

    # Events the script does not handle change nothing.
    assert run('leasefail') == [] and run('nak') == []
    print('PASS hisi dhcp: alias lease, router as default route, renewal, fixed gateway restored')
