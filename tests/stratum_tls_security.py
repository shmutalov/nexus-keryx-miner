"""Offline TLS authentication and shutdown fixtures. No mining or GPU use."""
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading

binary = str(Path(sys.argv[1]).resolve())
root = Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)

def cert(name, san):
    key, pem = root / (name + '.key'), root / (name + '.pem')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=synthetic-' + name, '-addext', 'subjectAltName=' + san, '-keyout', str(key), '-out', str(pem)], check=True, capture_output=True)
    return key, pem

valid = cert('valid', 'DNS:localhost,IP:127.0.0.1')
wrong = cert('wrong', 'DNS:wrong.invalid')
untrusted = cert('untrusted', 'DNS:localhost,IP:127.0.0.1')
expiry_ca = cert('expiry-ca', 'DNS:localhost')
expired = (expiry_ca[0], root / 'expired.pem')
subprocess.run(['openssl', 'req', '-new', '-key', str(expired[0]), '-subj', '/CN=synthetic-test', '-out', str(root / 'expired.csr')], check=True, capture_output=True)
(root / 'expiry-index').write_text('')
(root / 'expiry-serial').write_text('01\n')
(root / 'expiry.cnf').write_text(
    '[ca]\ndefault_ca=fixture\n[fixture]\n'
    + 'database="' + str(root / 'expiry-index') + '"\n'
    + 'serial="' + str(root / 'expiry-serial') + '"\n'
    + 'new_certs_dir="' + str(root) + '"\n'
    + 'default_md=sha256\npolicy=fixture_policy\nx509_extensions=fixture_ext\n'
    + '[fixture_policy]\ncommonName=supplied\n[fixture_ext]\nsubjectAltName=DNS:localhost\n')
subprocess.run(['openssl', 'ca', '-selfsign', '-batch', '-notext', '-config', str(root / 'expiry.cnf'), '-cert', str(expiry_ca[1]), '-keyfile', str(expired[0]), '-in', str(root / 'expired.csr'), '-startdate', '20200101000000Z', '-enddate', '20200102000000Z', '-out', str(expired[1])], check=True, capture_output=True)

def run(pair, host, expected, mode='tls_probe', ca_file=None, system_ca=None):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(pair[1]), str(pair[0]))
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(128)
    listener.settimeout(.1)
    stop = threading.Event()
    workers = []
    def serve(raw):
        try:
            raw.settimeout(2)
            with context.wrap_socket(raw, server_side=True) as conn:
                while not stop.is_set() and conn.recv(8192):
                    pass
        except (ssl.SSLError, OSError):
            raw.close()
    def accept():
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            t = threading.Thread(target=serve, args=(conn,))
            workers.append(t)
            t.start()
    thread = threading.Thread(target=accept)
    thread.start()
    env = dict(os.environ, SSL_CERT_FILE=str(valid[1] if pair == untrusted else pair[1]))
    if system_ca is not None: env['SSL_CERT_FILE'] = str(system_ca)
    if ca_file is not None: env['NEXUS_POOL_CA_FILE'] = str(ca_file)
    else: env.pop('NEXUS_POOL_CA_FILE', None)
    url = 'stratum+ssl://' + host + ':' + str(listener.getsockname()[1])
    try:
        args = [binary, mode, url] + ([expected] if mode == 'tls_probe' else [os.environ.get('NEXUS_TLS_STOP_ITERATIONS', '100')])
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=240)
        (root / (mode + '-' + host.replace(':', '_') + '-' + expected + '.log')).write_text(result.stdout + result.stderr)
        if mode == 'tls_stop' and ('TLS_STOP_CHECKS=' + args[-1]) not in result.stdout:
            raise RuntimeError('TLS stress did not attest the requested iteration count')
        if result.returncode:
            raise RuntimeError('exit=' + str(result.returncode) + '\n' + result.stdout + result.stderr)
    finally:
        stop.set()
        thread.join()
        listener.close()
        for worker in workers:
            worker.join()
    print('TLS_FIXTURE=PASS', mode, host, expected)

run(valid, 'localhost', 'accept')
run(valid, '127.0.0.1', 'accept')
run(wrong, 'localhost', 'reject')
run(wrong, '127.0.0.1', 'reject')
run(untrusted, 'localhost', 'reject')
run(expired, 'localhost', 'reject')
run(valid, 'localhost', 'accept', 'tls_stop')

# Extra roots supplement system roots without weakening hostname checks.
run(valid, 'localhost', 'accept', ca_file=valid[1], system_ca=untrusted[1])
run(wrong, 'localhost', 'reject', ca_file=wrong[1], system_ca=untrusted[1])
run(valid, 'localhost', 'reject', ca_file=root/'missing.pem')
(root/'malformed.pem').write_text('not a certificate')
run(valid, 'localhost', 'reject', ca_file=root/'malformed.pem')
