import datetime
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import time
from contextlib import closing

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'trafficmon_m1.exe'


def run(*args):
    result = subprocess.run([str(EXE), *args], capture_output=True, text=True,
                            encoding='utf-8', errors='replace', timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='trafficmon-') as directory:
    db = pathlib.Path(directory) / 'history.sqlite3'
    run('1', '--no-etw', '--db', str(db))
    with closing(sqlite3.connect(db)) as connection:
        assert connection.execute('pragma user_version').fetchone()[0] == 3
        assert connection.execute('select count(*) from application_hourly').fetchone()[0] == 0
        assert connection.execute('select mode,closed from collection_session').fetchall() == [('connections', 1)]
        first = connection.execute('select sum(up),sum(down) from nic_hourly').fetchone()
    run('1', '--no-etw', '--db', str(db))
    with closing(sqlite3.connect(db)) as connection:
        second = connection.execute('select sum(up),sum(down) from nic_hourly').fetchone()
        assert all(b >= a for a, b in zip(first, second))
        assert connection.execute('select count(*) from collection_session where closed=1').fetchone()[0] == 2
    today = datetime.datetime.now().strftime('%Y-%m-%d')
    output = run('--day', today, '--db', str(db))
    assert 'connections' in output and 'local calendar' in output and 'ETW session started' not in output
    run('--month', today[:7], '--db', str(db))
    bad = subprocess.run([str(EXE), '--day', '2026-02-30', '--db', str(db)], capture_output=True)
    assert bad.returncode != 0
    with closing(sqlite3.connect(db)) as connection:
        assert connection.execute('select count(*) from collection_session').fetchone()[0] == 2

    monitor = subprocess.Popen([str(EXE), '8', '--db', str(db)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    traffic = subprocess.Popen([sys.executable, str(ROOT / 'tests' / 'gen_known_traffic.py')],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    _, traffic_error = traffic.communicate(timeout=20)
    assert traffic.returncode == 0, traffic_error
    stdout, stderr = monitor.communicate(timeout=30)
    assert monitor.returncode == 0, stdout + stderr
    if '[OK] ETW session started' not in stdout:
        raise AssertionError('ETW unavailable: fallback passed, live ETW test NOT passed')
    with closing(sqlite3.connect(db)) as connection:
        rows = connection.execute('select identity,sum(up),sum(down) from application_hourly group by identity').fetchall()
        python_rows = [r for r in rows if r[0].lower().endswith('python.exe')]
        unknown_rows = [r for r in rows if r[0].startswith('unknown:') and r[0].endswith(':' + str(traffic.pid))]
        assert sum(r[1] for r in python_rows + unknown_rows) == 1020, rows
        assert sum(r[2] for r in python_rows + unknown_rows) == 1020, rows
        assert connection.execute('select mode,closed from collection_session order by id desc limit 1').fetchone() == ('etw', 1)
        assert connection.execute('select count(*) from application_hourly where up_lan is null').fetchone()[0] == 0
        assert connection.execute('select count(*) from nic_hourly where up_lan is not null').fetchone()[0] == 0
        assert connection.execute('select count(*) from application_hourly '
                                  'where up_lan > up or down_lan > down').fetchone()[0] == 0
        print('Live application rows:', rows)

    # Short-lived processes: each sends one datagram and exits. Identity must come from the
    # Kernel-Process start event cache, not from a later (failing) live lookup.
    burst_db = pathlib.Path(directory) / 'burst.sqlite3'
    monitor = subprocess.Popen([str(EXE), '6', '--db', str(burst_db)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    time.sleep(2)
    bursts = []
    for _ in range(6):
        generator = subprocess.run([sys.executable, str(ROOT / 'tests' / 'gen_burst_traffic.py')],
                                   capture_output=True, text=True, timeout=20)
        assert generator.returncode == 0, generator.stderr
        pid, size = generator.stdout.split()
        bursts.append((int(pid), int(size)))
    stdout, stderr = monitor.communicate(timeout=30)
    assert monitor.returncode == 0, stdout + stderr
    if '[OK] ETW session started' not in stdout:
        raise AssertionError('ETW unavailable: short-lived identity test NOT passed')
    with closing(sqlite3.connect(burst_db)) as connection:
        rows = connection.execute('select identity,sum(up) from application_hourly group by identity').fetchall()
        unknown = [r for r in rows if r[0].startswith('unknown:')]
        assert not unknown, rows
        python_rows = [r for r in rows if r[0].lower().endswith('python.exe')]
        assert sum(r[1] for r in python_rows) >= sum(size for _, size in bursts), rows
        print('Short-lived identity rows:', rows)

    print('PASS: fallback persistence, restart, local day/month, invalid date, no capture during query, '
          'live ETW exit flush, short-lived process identity')
