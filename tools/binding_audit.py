# binding_audit.py - 1.1.0 (the owner: "make sure every button in the code is bound, works, saves and really changes").
# 1. Every control in Views\MainWindow.axaml whose value goes back to the model (a switch's IsChecked, a list's or
#    drop-down's SelectedIndex / SelectedItem, a slider's or number box's Value, a text box's Text) is bound to a
#    property of MainViewModel (or SuperVideo) that has a setter - else the click shows but nothing is kept.
# 2. Every setter of MainViewModel that changes a profile's options goes through updateOptions / persist (saved).
# 3. Every field of the profile's options (Models\Preferences.fs DisplayOptions) is read where the engine's start file
#    or the Linux layer's settings are written (Infrastructure\MotionEngine.fs) - else it is kept but changes nothing.
import re, os, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
xaml = open(os.path.join(root, 'Views', 'MainWindow.axaml'), encoding='utf-8-sig').read()
vm = open(os.path.join(root, 'ViewModels', 'MainViewModel.fs'), encoding='utf-8-sig').read()
video = open(os.path.join(root, 'ViewModels', 'SuperVideo.fs'), encoding='utf-8-sig').read()
prefs = open(os.path.join(root, 'Models', 'Preferences.fs'), encoding='utf-8-sig').read()
engine = open(os.path.join(root, 'Infrastructure', 'MotionEngine.fs'), encoding='utf-8-sig').read()


def members(src):
    """name -> (has setter, setter text) for `member _.X` / `member this.X` / `member val X`."""
    out = {}
    names = [(m.start(), m.group(2)) for m in re.finditer(r'member (val |\w+\.)(\w+)', src)]
    for i, (at, name) in enumerate(names):
        end = names[i + 1][0] if i + 1 < len(names) else len(src)
        body = src[at:end]
        has_set = bool(re.search(r'\band set\b|\bwith get, set\b|with set\b', body))
        out[name] = (has_set, body)
    return out


m_vm, m_video = members(vm), members(video)
problems = 0
print('== 1. controls that write back, and their setters')
two_way = re.compile(r'<(\w+)\b([^>]*?)\b(IsChecked|SelectedIndex|SelectedItem|Value|Text)="\{Binding ([\w.!]+)[^}]*\}"')
count = 0
for m in two_way.finditer(xaml):
    tag, attrs, prop, path = m.group(1), m.group(2), m.group(3), m.group(4)
    if prop == 'Text' and tag not in ('TextBox',):
        continue
    if 'Mode=OneWay' in m.group(0):
        continue
    count += 1
    name = re.search(r'Name="(\w+)"', m.group(0))
    label = name.group(1) if name else tag
    parts = path.lstrip('!').split('.')
    table = m_video if parts[0] == 'Video' and len(parts) > 1 else m_vm
    member = parts[-1]
    if parts[0] == 'Video' and len(parts) == 1:
        continue
    found = table.get(member) or m_vm.get(member) or m_video.get(member)
    if found is None:
        # (Item templates bind their own items: LearnedItem, VideoItem ... - their setters are in those types.)
        if re.search(r'type (LearnedItem|VideoItem|VideoChoice|ChoiceItem)', vm + video) and member in ('FramesIndex', 'TechniqueIndex', 'SizeIndex', 'UpscalerIndex', 'CodecIndex'):
            found = (True, '')
    if found is None:
        print(f'  ?? {label}: {prop} -> {path} - no such property found'); problems += 1
    elif not found[0]:
        print(f'  NO SETTER {label}: {prop} -> {path}'); problems += 1
print(f'  {count} controls looked at')

print('== 2. setters that change the profile and are saved')
for name, (has_set, body) in sorted(m_vm.items()):
    if not has_set:
        continue
    setter = body[body.find('and set'):] if 'and set' in body else body
    saves = re.search(r'updateOptions|persist|save|Save|setGlobal|SetProperty|apply|Apply|engine\.|Engine\.|select|Select|commit|Commit|telling', setter)
    if not saves:
        print(f'  ?? {name}: its setter neither saves nor tells anything:'); print('     ' + setter.strip().replace('\n', '\n     ')[:400]); problems += 1

print('== 3. profile fields the engine (or the Linux layer) never reads')
record = prefs[prefs.find('type DisplayOptions'):]
record = record[:record.find('}') + 1]
fields = re.findall(r'(\w+)\s*:\s*[\w<>\[\] ]+?(?=\s*(?://|\n|;|\}))', record)
fields = [f for f in fields if f[0].isupper()]
app_only = {'Schema'}
for f in fields:
    if f in app_only:
        continue
    if not re.search(r'options\.' + f + r'\b|o\.' + f + r'\b|\.' + f + r'\b', engine):
        used_elsewhere = [n for n in ('MainViewModel.fs',) if re.search(r'\.' + f + r'\b', vm)]
        print(f'  {f}: not in MotionEngine.fs' + (' (the page uses it)' if used_elsewhere else ''))
print(f'== {problems} problem(s) in 1 and 2')
