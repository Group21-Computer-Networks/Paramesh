"""The translator (X-translator): each plain C program must print the same on two nodes as gcc's
own build prints alone, and each loop must end up where the case says. Needs PMRUN, STATE, PREFIX."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/playground"))
from translate import translate

failures = 0
ONE = "#include <stdio.h>\n#include <stdlib.h>\n#include <math.h>\n"


def check(name, good, detail=""):
    global failures
    print(("ok   " if good else "FAIL ") + name, detail if not good else "")
    failures += 0 if good else 1


# name, program (main's body unless it holds main), lines that run on the pool
CASES = [
    ("a total", "int n=100000; long s=0;\nfor(int i=0;i<n;i++){s+=i;}\nprintf(\"%ld\\n\",s);return 0;", [6]),
    ("a product", "unsigned long long f=1;\nfor(int k=1;k<=20;k++) f*=k;\nprintf(\"%llu\\n\",f);return 0;", [6]),
    ("a count, conditional", "int c=0;\nfor(int i=0;i<1000;i++){if(i%3==0)c++;}\nprintf(\"%d\\n\",c);return 0;", [6]),
    ("subtracting", "long s=500;\nfor(int i=0;i<100;i++){s-=i;}\nprintf(\"%ld\\n\",s);return 0;", [6]),
    ("a fixed array, read later", "int a[5000];\nfor(int i=0;i<5000;i++){a[i]=i*2;}\nlong s=0;\nfor(int i=0;i<5000;i++){s+=a[i];}\nprintf(\"%ld %d\\n\",s,a[77]);return 0;", [6, 8]),
    ("malloc and free", "int n=4000;double*a=malloc(n*sizeof(double));\nfor(int i=0;i<n;i++){a[i]=sqrt(i);}\nprintf(\"%.6f\\n\",a[99]);free(a);return 0;", [6]),
    ("calloc", "int n=4000;int*a=calloc(n,sizeof(int));\nfor(int i=1;i<n;i++){a[i]=i;}\nprintf(\"%d %d\\n\",a[0],a[3999]);free(a);return 0;", [6]),
    ("2D product", "#define N 80\n" + ONE + "double a[N][N],b[N][N],c[N][N];\nint main(void){int i,j,k;\nfor(i=0;i<N;i++){for(j=0;j<N;j++){a[i][j]=i+j;b[i][j]=i-j;}}\nfor(i=0;i<N;i++){for(j=0;j<N;j++){double s=0;for(k=0;k<N;k++){s+=a[i][k]*b[k][j];}c[i][j]=s;}}\nprintf(\"%.1f\\n\",c[9][70]);return 0;}\n", [7, 8]),
    ("loop variable read afterwards", "int i;long s=0;\nfor(i=3;i<50;i++){s+=i;}\nprintf(\"%d %ld\\n\",i,s);return 0;", [6]),
    ("empty range", "int n=0;long s=7;\nfor(int i=5;i<n;i++){s+=i;}\nprintf(\"%ld\\n\",s);return 0;", [6]),
    ("scratch set first", "double x;double s=0;long c=0;\nfor(int i=0;i<1000;i++){x=i*0.5;if(x>100)c++;}\nprintf(\"%ld\\n\",c);return 0;", [6]),
    ("pure helper", "#include <stdio.h>\nstatic long sq(long v){return v*v;}\nint main(void){long s=0;\nfor(int i=0;i<999;i++){s+=sq(i);}\nprintf(\"%ld\\n\",s);return 0;}\n", [4]),
    ("inner loop when the outer prints", "int a[3000];\nfor(int t=0;t<2;t++){\nfor(int i=0;i<3000;i++){a[i]=i+t;}\nprintf(\"%d\\n\",a[10]);}\nreturn 0;", [7]),
    ("order matters", "int a[100];a[0]=1;\nfor(int i=1;i<100;i++){a[i]=(a[i-1]*3)%7;}\nprintf(\"%d\\n\",a[99]);return 0;", []),
    ("depends on the last step", "long x=1;\nfor(int i=0;i<20;i++){x=x*2+i;}\nprintf(\"%ld\\n\",x);return 0;", []),
    ("prints", "for(int i=0;i<3;i++){printf(\"%d\\n\",i);}\nreturn 0;", []),
    ("stops early", "int f=-1;int a[50];\nfor(int i=0;i<50;i++){a[i]=i*i;}\nfor(int i=0;i<50;i++){if(a[i]>100){f=i;break;}}\nprintf(\"%d\\n\",f);return 0;", [6]),
    ("histogram", "int h[10]={0};\nfor(int i=0;i<1000;i++){h[i%10]++;}\nprintf(\"%d\\n\",h[3]);return 0;", []),
    ("impure helper", "#include <stdio.h>\nint calls=0;\nstatic int f(int v){calls++;return v;}\nint main(void){long s=0;\nfor(int i=0;i<10;i++){s+=f(i);}\nprintf(\"%ld %d\\n\",s,calls);return 0;}\n", []),
    ("writes by helper", "#include <stdio.h>\nstatic void f(int*p,int i){p[i]=i;}\nint main(void){int a[9];\nfor(int i=0;i<9;i++){f(a,i);}\nprintf(\"%d\\n\",a[8]);return 0;}\n", []),
    ("a product under a modulus", "long f=1;\nfor(int k=1;k<=20;k++){f=f*k%1000003;}\nprintf(\"%ld\\n\",f);return 0;", []),
    ("a shift after a sum", "long x=1;\nfor(int k=1;k<=9;k++){x=x+k<<1;}\nprintf(\"%ld\\n\",x);return 0;", []),
    ("rand", "srand(1);long s=0;\nfor(int i=0;i<5;i++){s+=rand()%10;}\nprintf(\"%ld\\n\",s);return 0;", []),
    ("struct", "struct P{int x;};struct P p={3};long s=0;\nfor(int i=0;i<9;i++){s+=p.x;}\nprintf(\"%ld\\n\",s);return 0;", []),
    ("aliased pointer", "int*a=malloc(400);int*b=a;\nfor(int i=1;i<100;i++){a[i]=i;}\nfor(int i=1;i<100;i++){b[i]=b[i]+1;}\nprintf(\"%d\\n\",a[50]);return 0;", []),
    ("total read in the loop", "long s=0;int a[20];\nfor(int i=0;i<20;i++){s+=i;a[i]=s;}\nprintf(\"%d\\n\",a[19]);return 0;", []),
    ("counts down", "long s=0;\nfor(int i=9;i>=0;i--){s+=i;}\nprintf(\"%ld\\n\",s);return 0;", []),
]


def wrap(program):
    return program if "int main" in program else ONE + "int main(void){\n" + program + "\n}\n"


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=40)


prefix = os.environ["PREFIX"]
for name, program, pool in CASES:
    source = wrap(program)
    translated, notes = translate(source)
    lines = sorted(int(n.split()[1]) for n in notes if "runs on the pool" in n)
    check(f"{name}: the loops chosen", lines == pool, f"{lines} != {pool}: {notes}")
    if not pool:
        check(f"{name}: left as it was", translated == source)
        continue
    with tempfile.TemporaryDirectory() as directory:
        plain, split = Path(directory) / "plain.c", Path(directory) / "split.c"
        plain.write_text(source)
        split.write_text(translated)
        built = run(["gcc", "-std=gnu17", "-O2", str(plain), "-o", f"{directory}/plain", "-lm"])
        expected = run([f"{directory}/plain"]).stdout
        built = run(["gcc", "-std=gnu17", "-O2", "-static", str(split), "-o", f"{directory}/split",
                     f"-I{prefix}/include", f"-L{prefix}/lib", "-lparamesh", "-lstdc++", "-lm", "-lpthread"])
        if built.returncode != 0:
            check(f"{name}: compiles", False, built.stderr[:400])
            continue
        command = [os.environ["PMRUN"], "-n", "2", "--state-dir", os.environ["STATE"], f"{directory}/split"]
        got = run(command)
        check(f"{name}: same output", got.stdout == expected, f"{got.stdout!r} != {expected!r} {got.stderr[-300:]}")
sys.exit(1 if failures else 0)
