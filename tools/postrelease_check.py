"""发完版对着**公开那一面**复验三件事（不看 CI 日志，直接打 GitHub 的公开 URL）：
1. Release 说明里有没有家目录/用户名 —— 用的就是发版说明那把同一颗图案；
2. 默认安装器不附带插件；显式分发兼容插件时，复验清单已上传且条目数符合所发标签；
3. `sha256.txt` 里安装包那一行，和 Release 资产 API 报的 digest 对不对得上。

    python tools/postrelease_check.py               # 量"HEAD 上最近那个 tag"那一版
    python tools/postrelease_check.py --tag v1.4.1  # 点名量某一版
    python tools/postrelease_check.py 已经下好的.json  # 离线：复用 release JSON，仍会拉取校验文件

版本号以前钉死在 `v1.4.0`：发完 1.4.1 之后它还在复验上一版 —— 那是"绿了但量错了对象"，
所以这里改成从 `git describe --tags` 问出来，不在这儿抄第二份。
"""
import glob
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
release_repo = 'EienSakura/hotaru-launcher'


def get(url, tries=4):
    """公开 URL 一律走 curl，不走 python 的 urllib。
    这台机上的 msys python 没有可信 CA 那份（`CERTIFICATE_VERIFY_FAILED: unable to get local
    issuer certificate`），而 GitHub 的 443 又时不时直接被掐（`远程主机强迫关闭了一个现有的连接`）——
    两个都是环境的事，不是"那条链没活"。curl 自带 CA 包，所以拿它量，并且重试几次。

    Windows 上还要多一颗：资产那一跳会 302 到 CDN 主机，schannel 拿不到吊销服务器就整条
    `rc=35 CRYPT_E_REVOCATION_OFFLINE` —— 直连和经代理都一样，而 api.github.com 那一跳没事，
    所以很容易被读成"链没活"。只在 Windows 上关吊销检查（证书链本身照样验）。"""
    revoke = ['--ssl-no-revoke'] if sys.platform == 'win32' else []
    last = ''
    for i in range(tries):
        r = subprocess.run(['curl', '-fsSL', '--max-time', '40'] + revoke + ['-o', '-', url],
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout
        last = 'curl rc=%d %s' % (r.returncode, (r.stderr or '').strip()[:120])
        time.sleep(2 + 2 * i)
    raise RuntimeError(last)


def nearest_tag():
    r = subprocess.run(['git', 'describe', '--tags', '--abbrev=0'], cwd=root,
                       capture_output=True, text=True)
    return (r.stdout or '').strip()


def expected_plugins(tag=''):
    """那一版发出去时有几颗插件：**从那个 tag 数**，不从工作树数。
    第一版这里是现算工作树，于是拿 v1.4.1 那次发版（19 条）比今天（20 颗）报了一条 PROBLEM ——
    那是把"过去的产物"和"未来的树"放在一起比，量错了对象。tag 不在本地时退回现算并说清楚。"""
    if tag:
        r = subprocess.run(['git', 'ls-tree', '-r', '--name-only', tag, '--'], cwd=root,
                           capture_output=True, text=True)
        names = [l for l in (r.stdout or '').splitlines()
                 if l.startswith('plugins/') and l.endswith('/plugin.json')]
        if names:
            return len(names)
    return len(glob.glob(os.path.join(root, 'plugins', '*', 'plugin.json')))


spec = importlib.util.spec_from_file_location('mrn', os.path.join(here, 'make_release_notes.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

argv = list(sys.argv[1:])
tag = ''
if '--tag' in argv:
    i = argv.index('--tag')
    tag = argv[i + 1]
    del argv[i:i + 2]

if argv:                                    # 离线：拿已经下好的那份 release JSON
    rel = json.load(open(argv[0], encoding='utf-8'))
    tag = tag or rel.get('tag_name', '')
else:
    tag = tag or nearest_tag()
    if not tag:
        print('PROBLEM 不知道要量哪一版（这里没有 tag，也没给 --tag）')
        sys.exit(1)
    rel = json.loads(get(f'https://api.github.com/repos/{release_repo}/releases/tags/{tag}'))

if not tag or rel.get('tag_name') != tag or rel.get('draft'):
    print('PROBLEM 标签与正式发布不匹配')
    sys.exit(1)

assets = {asset['name']: asset for asset in rel.get('assets', [])}
bad = 0

# 发布说明不得携带本机家目录。
hits = m.USER_HOME_RE.findall(rel.get('body', '') or '')
print('说明里带家目录的行=', len(hits))
bad += len(hits)

index_asset = assets.get('plugins-index.json')
if index_asset:
    try:
        index = json.loads(get(index_asset['browser_download_url']))
        plugins = index.get('plugins', [])
        wanted = expected_plugins(tag)
        print('兼容插件清单条目=', len(plugins))
        if len(plugins) != wanted:
            print('PROBLEM 清单条目数与标签中的兼容插件数不同：', wanted)
            bad += 1
    except (RuntimeError, ValueError, KeyError, AttributeError) as exc:
        print('PROBLEM 兼容插件清单不可读：', exc)
        bad += 1
elif any(name.startswith('ltool-') for name in assets):
    print('PROBLEM 发布包含兼容插件资产但缺少清单')
    bad += 1
else:
    print('未分发兼容插件，符合默认安装包约定')

installer = f"Hotaru-{tag.removeprefix('v')}-setup.exe"
setup = assets.get(installer)
hashes = assets.get('sha256.txt')
if setup and hashes:
    try:
        txt = get(hashes['browser_download_url'])
        expected = re.findall(rf"^([0-9a-fA-F]{{64}})  {re.escape(installer)}$", txt, re.M)
        actual = setup.get('digest') or ''
        if len(expected) != 1 or actual.lower() != f'sha256:{expected[0].lower()}':
            print('PROBLEM 安装包资产摘要与 sha256.txt 不一致')
            bad += 1
        else:
            print('安装包 SHA-256 已核对：', installer)
    except (RuntimeError, KeyError) as exc:
        print('PROBLEM 校验表不可读：', exc)
        bad += 1
else:
    print('PROBLEM 缺', installer, '或 sha256.txt 资产')
    bad += 1

print('POSTRELEASE_BAD=%d' % bad)
sys.exit(1 if bad else 0)
