#!/usr/bin/env python3
"""生成显式额外打包时使用的插件清单。

默认构建和发布不调用此脚本。启用额外插件构建后，从产物计算 SHA-256 和字节数，
从各插件 plugin.json 读取描述、版本和 API，生成 Hotaru 仓库对应标签下的下载地址。
此清单不会自动替换运行时保留的上游插件源；功能适配仍需自行验证。

用法：python tools/make_plugin_index.py --build <DLL目录> --out <清单路径>
可选 --base-url <下载前缀>；默认 https://github.com/EienSakura/Hotaru/releases/download/v<版本>。
缺少产物或 API 不符时非零退出，不生成不完整的清单。
"""
import argparse
import glob
import hashlib
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 额外插件资产与 Hotaru 当前标签绑定；运行时默认商店源独立维护。
RELEASE_BASE = 'https://github.com/EienSakura/Hotaru/releases/download'


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def read(path):
    return io.open(path, encoding='utf-8', errors='replace').read()


def core_label():
    """版本号唯一来源还是 ltool.iss，接口号是头里那个。'来自 Hotaru 1.0.0（插件接口 API 4）'"""
    ver = re.search(r'#define MyAppVersion "([^"]+)"', read(os.path.join(ROOT, 'ltool.iss'))).group(1)
    api = int(re.search(r'#define\s+LTOOL_PLUGIN_API_VERSION\s+(\d+)',
                        read(os.path.join(ROOT, 'src', 'ltool-plugin.h'))).group(1))
    return ver, api, '来自 Hotaru %s（插件接口 API %d）' % (ver, api)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True, help='编好的 dll 所在目录')
    ap.add_argument('--src', default=os.path.join(ROOT, 'plugins'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--base-url', default='', help='产物下载前缀；留空 = 本次发版标签那一档')
    a = ap.parse_args()

    ver, api_now, core = core_label()
    base = a.base_url.strip() or '%s/v%s' % (RELEASE_BASE, ver)
    rows = []
    problems = []
    for pj in sorted(glob.glob(os.path.join(a.src, '*', 'plugin.json'))):
        meta = json.loads(read(pj))
        name = 'ltool-%s.dll' % meta['Id']
        dll = os.path.join(a.build, name)
        if not os.path.exists(dll):
            problems.append('%s：有 plugin.json，产物里没有 %s —— 构建没成功就别出清单'
                            % (meta['Id'], name))
            continue
        # plugin.json 里那个 Api 是插件自报的接口号，宿主拿它放行；对不上头里的数就是写错了
        if int(meta.get('Api', -1)) != api_now:
            problems.append('%s：plugin.json 写的 Api=%s，头里是 %d'
                            % (meta['Id'], meta.get('Api'), api_now))
            continue
        row = {'id': meta['Id'], 'name': meta['Name'], 'desc': meta['Desc'],
               'api': meta['Api'], 'keywords': meta.get('Keywords', []),
               'actions': meta.get('Actions', []),
               'version': meta['Version'], 'color': meta.get('Color', ''),
               'file': name, 'bytes': os.path.getsize(dll), 'sha256': sha256_of(dll)}
        row['url'] = base.rstrip('/') + '/' + name
        rows.append(row)

    if problems:
        for p in problems:
            print('FAIL ' + p)
        print('INDEX_PLUGINS=%d' % len(rows))
        print('INDEX_BAD=%d' % len(problems))
        return 1

    doc = {'schema': 1, 'core': core, 'plugins': rows}
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    io.open(out, 'w', encoding='utf-8').write(json.dumps(doc, ensure_ascii=False, indent=1))
    print('清单：%d 颗 -> %s（%s）' % (len(rows), out, core))
    for r in rows:
        print('  %-9s v%-7s api=%s %8d B  %s' % (r['id'], r['version'], r['api'],
                                                 r['bytes'], r['sha256'][:12]))
    print('INDEX_PLUGINS=%d' % len(rows))
    print('INDEX_BAD=0')
    return 0


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.exit(main())
