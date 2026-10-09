import re, os
p = os.path.expandvars(r"%TEMP%\opencode\preview3\index.html")
s = open(p, encoding='utf-8').read()
m = re.search(r'<meta http-equiv="refresh" content="([^"]*)"', s)
print("  refresh content now:", m.group(1))
