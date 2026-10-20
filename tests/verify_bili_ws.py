"""验证 B站弹幕协议：构造认证包，连真实服务器，看能否收到回包。
用法：
  python verify_bili_ws.py [auth.txt] [room_id]
auth.txt 为插件保存的 Cookie 行；不传则用游戏目录下的 auth.txt。
"""
import socket, ssl, struct, json, urllib.request, time, sys, os

ROOM = int(sys.argv[2]) if len(sys.argv) > 2 else 123456
SALT = 'ea1db124af3c7062474693fa704f4ff8'

AUTH_PATH = sys.argv[1] if len(sys.argv) > 1 else r'auth.txt'
COOKIE = ''
UID = 0
BUVID = ''
if os.path.exists(AUTH_PATH):
    COOKIE = open(AUTH_PATH, 'r', encoding='utf-8').read().strip()
    for part in COOKIE.split(';'):
        k, _, v = part.strip().partition('=')
        if k == 'DedeUserID':
            UID = int(v or 0)
        elif k == 'buvid3':
            BUVID = v
print('auth file:', AUTH_PATH, 'cookie:', bool(COOKIE), 'uid:', UID, 'buvid:', bool(BUVID))

def md5(s):
    import hashlib
    return hashlib.md5(s.encode()).hexdigest()

import urllib.parse
WBI_INDEX = [
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
]

def wbi_key():
    h = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126'}
    if COOKIE:
        h['Cookie'] = COOKIE
    try:
        d = json.load(urllib.request.urlopen(
            urllib.request.Request('https://api.bilibili.com/x/web-interface/nav', headers=h),
            timeout=15))
        img = d['data']['wbi_img']
        a = img['img_url'].rpartition('/')[2].partition('.')[0]
        b = img['sub_url'].rpartition('/')[2].partition('.')[0]
        k = a + b
        return ''.join(k[i] for i in WBI_INDEX if i < len(k))
    except Exception as e:
        print('wbi fetch failed:', e)
        return ''

def wbi_sign(params, key):
    params = {**params, 'wts': str(int(time.time()))}
    params = {k: ''.join(ch for ch in str(v) if ch not in "!'()*")
              for k, v in sorted(params.items())}
    q = urllib.parse.urlencode(params)
    return q + '&w_rid=' + md5(q + key)

# 1) getDanmuInfo（WBI 签名）
key = wbi_key()
print('wbi key len', len(key))
if key:
    q = wbi_sign({'id': ROOM, 'type': 0}, key)
else:
    wts = int(time.time())
    q = 'id=%d&type=0&wts=%d&w_rid=%s' % (ROOM, wts, md5('id=%d&type=0&wts=%d' % (ROOM, wts) + SALT))
url = ('https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?%s' % q)
hdrs = {
    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126',
    'Referer': 'https://live.bilibili.com/',
    'Origin': 'https://live.bilibili.com',
}
if COOKIE:
    hdrs['Cookie'] = COOKIE
req = urllib.request.Request(url, headers=hdrs)
info = json.load(urllib.request.urlopen(req, timeout=15))
print('code', info.get('code'))
data = info.get('data') or {}
token = data.get('token', '')
hosts = data.get('host_list') or []
print('token len', len(token), 'hosts', len(hosts))
if not token or not hosts:
    print('无法继续', info)
    raise SystemExit(1)
host = hosts[0]['host']
port = hosts[0].get('wss_port', 443)
print('host', host, port)

# 2) WebSocket 握手
ctx = ssl.create_default_context()
raw = socket.create_connection((host, port), timeout=10)
s = ctx.wrap_socket(raw, server_hostname=host)
key = 'dGhlIHNhbXBsZSBub25jZQ=='
cookie_hdr = ('Cookie: %s\r\n' % COOKIE) if COOKIE else ''
hreq = ('GET /sub HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
        'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n'
        'Origin: https://live.bilibili.com\r\n%s'
        'User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126\r\n\r\n'
        % (host, key, cookie_hdr))
s.sendall(hreq.encode())
buf = b''
while b'\r\n\r\n' not in buf:
    d = s.recv(4096)
    if not d:
        print('handshake closed'); raise SystemExit(1)
    buf += d
head, rest = buf.split(b'\r\n\r\n', 1)
print('--- handshake ---')
print(head.decode('latin1')[:200])

# 3) 发认证包
def mkframe(payload):
    # client 必须掩码
    import os
    mask = os.urandom(4)
    ln = len(payload)
    h = bytearray([0x82])
    if ln < 126:
        h.append(0x80 | ln)
    elif ln < 65536:
        h.append(0x80 | 126); h += struct.pack('>H', ln)
    else:
        h.append(0x80 | 127); h += struct.pack('>Q', ln)
    h += mask
    body = bytearray(payload[i] ^ mask[i % 4] for i in range(ln))
    return bytes(h) + bytes(body)

auth = json.dumps({'uid': UID, 'roomid': ROOM, 'protover': 3, 'platform': 'web',
                   'type': 2, 'buvid': BUVID, 'key': token}, separators=(',', ':'))
pkt = struct.pack('>IHHII', 16 + len(auth), 16, 1, 7, 1) + auth.encode()
print('\n--- auth packet len', len(pkt), '---')
s.sendall(mkframe(pkt))
print('auth sent')

# 4) 读回应
s.settimeout(12)
def rdframe(pre):
    d = pre
    while len(d) < 2:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError('connection closed by peer')
        d += chunk
    b1, b2 = d[0], d[1]
    ln = b2 & 0x7f
    idx = 2
    if ln == 126:
        while len(d) < 4:
            chunk = s.recv(4096)
            if not chunk: raise ConnectionError('closed')
            d += chunk
        ln = struct.unpack('>H', d[2:4])[0]; idx = 4
    elif ln == 127:
        while len(d) < 10:
            chunk = s.recv(4096)
            if not chunk: raise ConnectionError('closed')
            d += chunk
        ln = struct.unpack('>Q', d[2:10])[0]; idx = 10
    while len(d) < idx + ln:
        chunk = s.recv(65536)
        if not chunk: raise ConnectionError('closed')
        d += chunk
    return b1 & 0x0f, d[idx:idx+ln], d[idx+ln:]

pre = rest
for i in range(4):
    try:
        op, pl, pre = rdframe(pre)
    except Exception as e:
        print('read end:', e); break
    print('\n--- frame %d opcode=%d len=%d ---' % (i, op, len(pl)))
    print('hex head:', pl[:32].hex(' '))
    if len(pl) >= 16:
        total, hlen, ver, opcode = struct.unpack_from('>IHHI', pl, 0)
        print('pkt total=%d hlen=%d ver=%d op=%d' % (total, hlen, ver, opcode))
        body = pl[hlen:total]
        print('body head:', body[:60])
s.close()
