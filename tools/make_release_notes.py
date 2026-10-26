#!/usr/bin/env python3
r"""make_release_notes.py —— 生成 GitHub Release 那份说明（形状照 mateo-ki/vpn-parser 的发布说明）。

以前 CI 用的是 `gh release create --generate-notes`，GitHub 自己拼出来的是**裸提交标题列表**：
没有重点、没有原因、没有"哪颗文件是干什么的"。而这个仓库的提交信息本来就写着原因和实测数字
（一笔提交动辄几十行），全被 `--generate-notes` 丢了。这份脚本就是把它们重新摆成人看得懂的样子：

    # 🚀 Hotaru v<X> 发布说明 / Release Notes
    🎉 概述（几笔提交 + 前几条的重点）
    ## 🌟 本次改动        ← 一笔提交一节，正文里那些"- "行原样进来
    ## 📦 下载与安装      ← 资产表：每个文件是什么、多大、sha256 前 8 位
    **Full Changelog**: compare/<上一个标签>...<这个标签>

用法：python tools/make_release_notes.py <标签> <输出文件> [--include-plugins] [--theme-file <ZIP>]
读的全是本地东西（git log + installer/ + build/Release/ltool-plugins/ + plugins/*/plugin.json），
**一次网都不上**；写不出内容就非 0 退出，让 CI 红，而不是发一份空说明。
"""
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import contextlib
import io
import tempfile
import zipfile
from unittest.mock import patch, mock_open

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 「家目录 + 用户名」那一段的两种写法（Windows 反斜杠 / POSIX 正斜杠）。
# 用户名只吃 `[A-Za-z0-9._-]+`：这样 `C:\Users\Public\Desktop` 也会被换成 %USERPROFILE%\Public\...，
# 那条本来就是系统公共目录、不是任何人的家 —— 但它出现在发版页里同样没有意义，换了不亏。
USER_HOME_RE = re.compile(r'[A-Za-z]:[\\/]{1,2}[Uu]sers[\\/]{1,2}[A-Za-z0-9._-]+|(?<![\w/])(?:/[Uu]sers|/home)/[A-Za-z0-9._-]+')

# 一节配一个图标：按提交标题里的关键词挑，第一条命中的算。
# 这不是装饰 —— 一眼能看出那一节是修 bug、加功能还是动界面，正是那份参考说明好用的地方。
ICONS = (
    ('🐛', ('修', '漏', '坑', '假绿', '红', '闪退', '卡', '对不上', '悬空', '崩')),
    ('🧩', ('插件', '商店', 'dll', 'sdk', '清单', '自动更新')),
    ('🎨', ('界面', '样式', '颜色', '日间', '夜间', '卡片', '窗口', '按钮', '图标', '主题')),
    ('⚡', ('慢', '耗时', '性能', '内存', '并行', '缓存', '优化')),
    ('✅', ('闸', '判据', '自测', 'ci', '测试', '反向验')),
    ('📝', ('文档', 'readme', '说明', '注释')),
    ('✨', ('新增', '加了', '接入', '支持', '做成', '改成', '搬进')),
)


def git(*args):
    return subprocess.run(('git',) + args, cwd=ROOT, capture_output=True, text=True,
                          encoding='utf-8', errors='replace').stdout


def pick_icon(subject):
    s = subject.lower()
    for icon, keys in ICONS:
        if any(k in s for k in keys):
            return icon
    return '📦'


def prev_tag(tag):
    """这个标签之前最近的那个 v* 标签；没有就返回 None（第一版，整条历史都算进来）。"""
    # "上一个标签"必须是**历史上在这之前的那一个**，不是"最新的那一个"：
    # 拿 git describe 问 `<这个标签>^` 才对（第一版我用 tag --sort=-creatordate 取第一个不等于自己的，
    # 结果给 v1.0.3 找出个 v1.3.3 当上一个，区间反了 ⇒ 0 笔提交）。
    out = git('describe', '--abbrev=0', '--tags', tag + '^').strip()
    # 本地可能保留上游标签，个人远端只发布 Hotaru 标签；首版以分叉点为基线。
    base = open(os.path.join(ROOT, 'tools', 'upstream-base.txt'), encoding='ascii').read().strip()
    if not re.fullmatch(r'[0-9a-f]{40}', base):
        raise SystemExit('上游基线不是有效提交 ID')
    if not out:
        # 独立首提交没有父历史；上游基线仅供兼容检查，不列入发布日志。
        return base if git('merge-base', base, tag).strip() == base else None
    if git('merge-base', out, base).strip() == git('rev-parse', out + '^{commit}').strip():
        return base
    return out


def commits(rng):
    """%x1e 分隔每条提交、%x1f 分隔标题和正文 —— 提交信息里全是中文和换行，
    拿换行当分隔符一定会被咬（这仓库的规矩：分隔符挑一个不可能出现在正文里的字符）。"""
    raw = git('log', rng, '--no-merges', '--reverse',
              '--format=%s%x1f%b%x1e')
    out = []
    for chunk in raw.split('\x1e'):
        chunk = chunk.strip('\n')
        if not chunk.strip():
            continue
        head, _, body = chunk.partition('\x1f')
        # 标题也过一遍 scrub：`C:\Users\x\…` 这种串在标题里同样出现过。
        out.append((scrub(head.strip()), body.strip()))
    return out


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def human(n):
    return '%.2f MB' % (n / 1048576.0) if n >= 1048576 else '%.0f KB' % (n / 1024.0)


def api_version():
    """插件接口号只有一个来源：src/ltool-plugin.h 里那颗 #define。别在这儿抄一个数。"""
    with open(os.path.join(ROOT, 'src', 'ltool-plugin.h'), encoding='utf-8', errors='replace') as f:
        m = re.search(r'#define\s+LTOOL_PLUGIN_API_VERSION\s+(\d+)', f.read())
    return m.group(1) if m else '?'


def plugin_versions():
    out = {}
    for p in glob.glob(os.path.join(ROOT, 'plugins', '*', 'plugin.json')):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
            out[str(d.get('Id'))] = str(d.get('Version'))
        except Exception:
            continue
    return out


def scrub(text):
    """把带用户名的家目录换成 %USERPROFILE% 再进说明。

    为什么要这一道：这个仓库的提交正文本来就写着实测路径（`C:\\Users\\<名字>\\AppData\\...`），
    而这份说明是**公开 Release 页上最显眼的一块** —— 同样的字符串在 `git log` 里是一行历史，
    印到发版封面上就是另一回事了。只认"家目录那一段"，别的（`D:\\software\\ltool`、
    `%LOCALAPPDATA%`）是文档要的路径，原样留着。
    """
    return USER_HOME_RE.sub('%USERPROFILE%', text)


def body_lines(body):
    """提交正文原样搬进来，但保证每条都是 markdown 的一项：
    这个仓库的提交信息本来就按 `- ` 一行一条写，少数几行是续行（缩进的），把它们并回上一条。"""
    items = []
    for line in body.splitlines():
        if not line.strip():
            continue
        if line.startswith(('- ', '* ')):
            items.append(line[2:].rstrip())
        elif line.startswith(('  ', '\t')) and items:
            items[-1] += ' ' + line.strip()
        else:
            items.append(line.rstrip())
    if not items:
        items = ['（这一笔没写正文，标题就是全部。）']
    return [scrub(i) for i in items]


def self_check():
    """`--check`：脱敏自己的判据。放在 CI 那条"每推都跑"的线上，不是只在发版那一次 ——
    否则把用户名漏出去的那一笔要等到推 tag 才看得见（而那时已经发出去了）。"""
    bad = []

    def eq(got, want, why):
        if got != want:
            bad.append('%s：got=[%s] want=[%s]' % (why, got, want))

    baseline = 'a' * 40
    def fake_git(*args):
        if args[0] == 'describe':
            return ''
        if args[0] == 'merge-base':
            return ''
        return ''
    with patch('builtins.open', mock_open(read_data=baseline)), patch.dict(globals(), git=fake_git):
        eq(prev_tag('HEAD'), None, '独立首提交不继承上游发布区间')
    with patch('builtins.open', mock_open(read_data=baseline)), patch.dict(globals(), git=lambda *args: 'v1.0.0' if args[0] == 'describe' else ('v1.0.0-sha' if args[0] == 'rev-parse' else '')):
        eq(prev_tag('HEAD'), 'v1.0.0', '后续版本沿用 Hotaru 标签比较')

    eq(scrub(r'旧目录 C:\Users\zzname\AppData\Roaming\ltool 还在'),
       r'旧目录 %USERPROFILE%\AppData\Roaming\ltool 还在', 'Windows 家目录那一段')
    eq(scrub('posix 那边是 /home/zzname/projects/x'),
       'posix 那边是 %USERPROFILE%/projects/x', 'POSIX 家目录那一段')
    eq(scrub(r'D:\software\ltool\data'), r'D:\software\ltool\data', '不是家目录的路径不许动')
    eq(scrub(r'%LOCALAPPDATA%\Programs\ltool'), r'%LOCALAPPDATA%\Programs\ltool', '占位符写法不许动')
    eq(scrub('C:/Users/zzname/x'), '%USERPROFILE%/x', '正斜杠写法也要认（Qt 归一化之后就长这样）')
    # 整条链：正文里带用户名的那一笔，经过 body_lines 之后一项都不许剩下用户名
    items = body_lines('- 本机实测：983 个文件，旧目录 `C:\\Users\\zzname\\AppData\\Roaming\\ltool` 原样在那儿')
    if any('zzname' in i for i in items):
        bad.append('body_lines 漏了用户名：%s' % items)
    # 在隔离目录放入残留插件，验证默认发布不会顺手将其收入；不读取真实产物。
    with tempfile.TemporaryDirectory(prefix='hotaru-release-check-') as sandbox:
        installer = os.path.join(sandbox, 'installer')
        plugins = os.path.join(sandbox, 'build', 'Release', 'ltool-plugins')
        os.makedirs(installer)
        os.makedirs(plugins)
        setup = 'Hotaru-1.0.0-setup.exe'
        for name in (setup, 'Hotaru-11.0.0-setup.exe', 'ltool-sdk-1.0.0.zip'):
            with open(os.path.join(installer, name), 'wb') as f:
                f.write(b'fixture')
        dll = os.path.join(plugins, 'ltool-fixture.dll')
        with open(dll, 'wb') as f:
            f.write(b'plugin fixture')
        index = os.path.join(installer, 'plugins-index.json')
        with open(index, 'w', encoding='utf-8') as f:
            json.dump({'plugins': [{'file': 'ltool-fixture.dll', 'sha256': digest(dll)}]}, f)
        output = os.path.join(sandbox, 'notes.md')
        with patch.dict(globals(), ROOT=sandbox, prev_tag=lambda tag: None,
                        commits=lambda rng: [('feat(test): 验证发布范围', '')],
                        plugin_versions=lambda: {}, api_version=lambda: '4'):
            def generate(extra=()):
                with patch.object(sys, 'argv', ['make_release_notes.py', 'v1.0.0', output, *extra]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    return main()

            eq(generate(), 0, '默认发布不依赖插件资产')
            with open(os.path.join(installer, 'sha256.txt'), encoding='utf-8') as f:
                names = [line.split('  ', 1)[1].strip() for line in f]
            eq(names, [setup], '即使已有 SDK、清单和 DLL，默认校验表也只包含当前安装包')
            eq(generate(['--include-plugins']), 0, '显式额外打包接受完整插件资产')
            with open(os.path.join(installer, 'sha256.txt'), encoding='utf-8') as f:
                names = {line.split('  ', 1)[1].strip() for line in f}
            eq(names, {setup, 'ltool-sdk-1.0.0.zip', 'plugins-index.json', 'ltool-fixture.dll'},
               '显式额外包包含插件及 SDK，不包含历史安装包')
            with open(dll, 'ab') as f:
                f.write(b'changed')
            eq(generate(['--include-plugins']), 1, '插件哈希不一致拒绝额外打包')
            os.remove(index)
            eq(generate(), 0, '默认发布无需插件清单')
            eq(generate(['--include-plugins']), 1, '额外打包缺清单时失败')
            theme = os.path.join(sandbox, 'Hotaru-theme-fixture-1.0.0.zip')
            with zipfile.ZipFile(theme, 'w') as archive:
                archive.writestr('theme.json', json.dumps({'id': 'fixture', 'name': '示例主题', 'version': '1.0.0'}))
            eq(generate(['--theme-file', theme]), 0, '合法独立主题加入发布资产')
            with open(os.path.join(installer, 'sha256.txt'), encoding='utf-8') as f:
                names = {line.split('  ', 1)[1].strip() for line in f}
            eq(names, {setup, os.path.basename(theme)}, '校验表包含安装器与独立主题，不含插件')
            eq(generate(['--theme-file', theme, '--theme-file', theme]), 1, '重复主题附件拒绝发布')
            with zipfile.ZipFile(theme, 'w') as archive:
                archive.writestr('other.json', '{}')
            eq(generate(['--theme-file', theme]), 1, '缺主题清单时拒绝发布')
    print('NOTES_CHECK_TOTAL=19')
    print('NOTES_CHECK_BAD=%d' % len(bad))
    for b in bad:
        print('FAIL %s' % b)
    return 1 if bad else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--check':
        return self_check()
    args = sys.argv[1:]
    themes = []
    while '--theme-file' in args:
        index = args.index('--theme-file')
        if index + 1 >= len(args):
            print('FAIL --theme-file 缺少 ZIP 路径')
            return 2
        themes.append(args[index + 1])
        del args[index:index + 2]
    include_plugins = '--include-plugins' in args
    if include_plugins:
        args.remove('--include-plugins')
    if len(args) != 2:
        print('用法：python tools/make_release_notes.py <标签> <输出文件> [--include-plugins] [--theme-file <ZIP>]')
        return 2
    tag, out_path = args
    base = prev_tag(tag)
    rng = (base + '..' + tag) if base else tag
    cs = commits(rng)
    if not cs:
        print('FAIL %s 这个区间一笔提交都没有（标签指错了？），不发空说明' % rng)
        return 1

    ver = tag.lstrip('v')
    L = []
    L.append('# 🚀 Hotaru %s 发布说明 / Release Notes' % tag)
    L.append('')
    keys = []
    for subject, _ in cs[-3:][::-1]:          # 最近三笔当"本次重点"
        # 取标题里第一个短句当"重点"。但有的标题写成「1.0.3：检查更新不再打 api…」——
        # 冒号前只是个版本号，拿它当重点等于什么都没说（本机 dry-run 第一次就撞上），
        # 所以太短或整个是数字就往后挪一段。
        parts = [x.strip() for x in re.split(r'[：:，,——]', subject) if x.strip()]
        k = next((x for x in parts if len(x) >= 6 and not re.fullmatch(r'[\dvx.]+', x)), subject)
        if len(k) > 30:
            k = k[:29] + '…'
        keys.append(k)
    lead = '🎉 欢迎使用 **Hotaru %s**！本次共 **%d 笔改动**' % (ver, len(cs))
    if keys:
        lead += '，重点是 ' + '、'.join('**%s**' % k for k in keys)
    lead += '。下面每一节是一笔提交，原因和实测数字都在里面。'
    L.append(lead)
    L.append('')
    L.append('---')
    L.append('')
    L.append('## 🌟 本次改动')
    L.append('')
    for i, (subject, body) in enumerate(cs[::-1], 1):     # 最新的排最前
        L.append('### %s %d. %s' % (pick_icon(subject), i, subject))
        L.append('')
        for item in body_lines(body):
            L.append('- %s' % item)
        L.append('')
    L.append('---')
    L.append('')
    L.append('## 📦 下载与安装')
    L.append('')
    L.append('Windows 10 及以上（安装包已声明 `MinVersion=10.0`）。按用户安装，**不需要管理员**，'
             '默认装到 `%LOCALAPPDATA%\\Programs\\Hotaru`（可在向导中更改）；双击安装向导走完即可，托盘常驻，`Alt+Space` 呼出。')
    L.append('')
    L.append('| 文件 | 大小 | sha256 前 8 位 | 说明 |')
    L.append('| :--- | :--- | :--- | :--- |')
    full = []      # (发布名, 字节数, 完整 sha256, 说明)
    # 默认只收当前 Hotaru 安装器，不扫描本机已有插件；显式开关才纳入额外产物。
    setup = os.path.join(ROOT, 'installer', 'Hotaru-%s-setup.exe' % ver)
    if not os.path.isfile(setup):
        print('FAIL 没找到当前版本安装包：%s' % setup)
        return 1
    full.append((os.path.basename(setup), os.path.getsize(setup), digest(setup),
                 'Hotaru 安装包，默认不含插件；可选择安装目录'))
    if include_plugins:
        sdk = os.path.join(ROOT, 'installer', 'ltool-sdk-%s.zip' % ver)
        idx = os.path.join(ROOT, 'installer', 'plugins-index.json')
        dlls = sorted(glob.glob(os.path.join(ROOT, 'build', 'Release', 'ltool-plugins', 'ltool-*.dll')))
        if not os.path.isfile(sdk) or not os.path.isfile(idx) or not dlls:
            print('FAIL 额外打包需要当前版本 SDK、插件清单和 DLL 产物')
            return 1
        with open(idx, encoding='utf-8') as f:
            manifest = json.load(f)
        listed = {p['file']: p['sha256'] for p in manifest.get('plugins', [])}
        if set(listed) != {os.path.basename(p) for p in dlls} or any(
                listed[os.path.basename(p)].lower() != digest(p).lower() for p in dlls):
            print('FAIL 插件清单与额外 DLL 的名称或校验值不匹配')
            return 1
        full.append((os.path.basename(sdk), os.path.getsize(sdk), digest(sdk),
                     '额外插件 SDK：头文件与导入库'))
        full.append(('plugins-index.json', os.path.getsize(idx), digest(idx),
                     '本次额外构建的插件清单，不自动替换运行时默认上游源'))
        pvs = plugin_versions()
        api = api_version()
        for p in dlls:
            name = os.path.basename(p)
            pid = re.sub(r'^ltool-|\.dll$', '', name)
            full.append((name, os.path.getsize(p), digest(p),
                         '额外插件 `%s`（v%s，接口 API %s），使用前自行验证适配' % (pid, pvs.get(pid, '?'), api)))
    # 主题由维护者单独提供，不从源码或本机安装目录自动搜集。
    seen = {entry[0] for entry in full}
    for path in themes:
        name = os.path.basename(path)
        try:
            if not re.fullmatch(r'Hotaru-theme-[A-Za-z0-9._-]+\.zip', name) or name in seen:
                raise ValueError('主题附件命名无效或重复')
            with zipfile.ZipFile(path) as archive:
                if archive.testzip() is not None:
                    raise ValueError('主题 ZIP 校验失败')
                manifest = json.loads(archive.read('theme.json'))
                if not all(isinstance(manifest.get(k), str) and manifest[k].strip() for k in ('id', 'name', 'version')):
                    raise ValueError('主题清单缺少标识、名称或版本')
            seen.add(name)
            full.append((name, os.path.getsize(path), digest(path),
                         '独立主题 ZIP：%s v%s，配置 → 外观 → 导入主题包；素材权利归原作者'
                         % (manifest['name'], manifest['version'])))
        except (OSError, ValueError, KeyError, zipfile.BadZipFile):
            print('FAIL 独立主题附件无效，停止发布')
            return 1
    # `sha256.txt` 是**这份脚本生成**的：说明里写了"下 sha256.txt 对整批"，那它必须真在资产里，
    # 不能是一句指向不存在文件的话。
    txt = os.path.join(ROOT, 'installer', 'sha256.txt')
    with open(txt, 'w', encoding='utf-8', newline='\n') as f:
        for name, size, sh, desc in full:
            f.write('%s  %s\n' % (sh, name))
    full.append(('sha256.txt', os.path.getsize(txt), '',
                 '上面这些文件的 SHA-256 全表（`Get-FileHash -Algorithm SHA256` 逐行对得上）'))
    for name, size, sh, desc in full:
        L.append('| `%s` | %s | `%s` | %s |' % (name, human(size), sh[:8] or '—', desc))
    L.append('')
    L.append('一共 **%d 个文件**。校验：`Get-FileHash <文件> -Algorithm SHA256`，或下 `sha256.txt` 对整批。' % len(full))
    L.append('')
    L.append('当前插件商店沿用上游 ltool，尚未测试与 Hotaru 的适配情况，需自行适配。'
             '默认安装包不含插件，已有插件和配置不会被清空。')
    if themes:
        L.append('主题 ZIP 是独立可选附件，不包含在安装器和源码中。主题素材权利归原作者，源码的 Apache-2.0 许可证不涵盖第三方素材。')
    if include_plugins:
        L.append('本次额外提供插件及 SDK 资产；独立下载、安装，不会自动改变默认商店地址。')
    L.append('')
    L.append('---')
    L.append('')
    if base:
        repo = git('config', '--get', 'remote.origin.url').strip()
        m = re.search(r'github\.com[:/](.+?)(?:\.git)?$', repo)
        if m:
            L.append('**Full Changelog**: https://github.com/%s/compare/%s...%s' % (m.group(1), base, tag))
        else:
            L.append('**Full Changelog**: 上一个标签 %s → %s' % (base, tag))
    else:
        L.append('**Full Changelog**: 这是第一个带说明的版本（前面没有可对比的标签）。')
    L.append('')

    text = '\n'.join(L)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('INFO 发布说明写好了：%s（%d 笔提交 · %d 个资产 · %d 字符）'
          % (out_path, len(cs), len(full), len(text)))
    print('NOTES_COMMITS=%d NOTES_ASSETS=%d' % (len(cs), len(full)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
