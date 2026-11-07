param([switch]$Test, [switch]$Clean)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Continue'
$root  = $PSScriptRoot
$gcc   = Join-Path $root 'tools\w64devkit\bin\gcc.exe'
$bin   = Join-Path $root 'build'
$inc   = Join-Path $root 'src'

if ($Clean -and (Test-Path $bin)) { Remove-Item -Recurse -Force $bin }
New-Item -ItemType Directory -Force -Path $bin | Out-Null

if (!(Test-Path $gcc)) {
    throw "未找到便携工具链：$gcc`n请先把 w64devkit 解压到 tools\w64devkit"
}

$flags = @(
    '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-g', '-static',
    '-finput-charset=UTF-8', '-fexec-charset=UTF-8'
)

# ===== 第一步：编译运行时（无 CRT），再嵌入为字节数组 =====
Write-Host '编译运行时（无 CRT） ...'
$rtflags = @(
    '-ffreestanding', '-nostdlib', '-fno-pic', '-fno-pie', '-fno-stack-protector',
    '-fno-asynchronous-unwind-tables', '-fno-builtin', '-O2',
    '-finput-charset=UTF-8', '-fexec-charset=UTF-8'
)
$rtobj = Join-Path $bin 'runtime.obj'
& $gcc @rtflags -c (Join-Path $root 'runtime\runtime.c') -o $rtobj
if ($LASTEXITCODE -ne 0) { throw '运行时编译失败' }

$genDir = Join-Path $bin '生成'
New-Item -ItemType Directory -Force -Path $genDir | Out-Null
$genC = Join-Path $genDir 'runtime_obj.c'
$bytes = [System.IO.File]::ReadAllBytes($rtobj)
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/* 自动生成：运行时目标文件（build/runtime.obj）的字节数组，勿手工编辑 */')
[void]$sb.AppendLine('const unsigned char ch_runtime_obj[] = {')
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.Append(('0x{0:X2}' -f $bytes[$i]))
    if ($i -lt $bytes.Length - 1) { [void]$sb.Append(',') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine() } else { [void]$sb.Append(' ') }
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('const unsigned int ch_runtime_obj_len = sizeof(ch_runtime_obj);')
[System.IO.File]::WriteAllText($genC, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$sources = @(Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.c |
             ForEach-Object { $_.FullName })
$sources += $genC

Write-Host '编译图标与版本资源 ...'
$windres = Join-Path $root 'tools\w64devkit\bin\windres.exe'
$res = Join-Path $bin 'han.res'
& $windres -c 65001 -I (Join-Path $root 'icon') (Join-Path $root 'icon\han.rc') `
    -O coff -o $res
if ($LASTEXITCODE -ne 0) { throw '图标资源编译失败' }

Write-Host '编译 han.exe ...'
& $gcc @flags "-I$inc" @sources $res -o (Join-Path $bin 'han.exe')
if ($LASTEXITCODE -ne 0) { throw '编译器构建失败' }

Write-Host '编译 hanedit.exe（代码编辑器） ...'
$edflags = @(
    '-std=c11', '-Wall', '-Wextra', '-O2', '-g', '-static',
    '-finput-charset=UTF-8', '-fexec-charset=UTF-8', '-mwindows'
)
& $gcc @edflags (Join-Path $root 'editor\hanedit.c') $res `
    -o (Join-Path $bin 'hanedit.exe') -lcomctl32 -lcomdlg32 -lshell32 -ladvapi32
if ($LASTEXITCODE -ne 0) { throw '编辑器构建失败' }

if ($Test) {
    $lib = @($sources | Where-Object { $_ -notlike '*main.c' })

    Write-Host '编译词法单元测试 ...'
    & $gcc @flags "-I$inc" @lib (Join-Path $root 'tests\lexer_test.c') `
        -o (Join-Path $bin 'lexer_test.exe')
    if ($LASTEXITCODE -ne 0) { throw '词法测试构建失败' }
    & (Join-Path $bin 'lexer_test.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '词法单元测试未通过' }

    Write-Host '编译解析器测试 ...'
    & $gcc @flags "-I$inc" @lib (Join-Path $root 'tests\parser_test.c') `
        -o (Join-Path $bin 'parser_test.exe')
    if ($LASTEXITCODE -ne 0) { throw '解析器测试构建失败' }
    & (Join-Path $bin 'parser_test.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '解析器测试未通过' }

    Write-Host '编译语义测试 ...'
    & $gcc @flags "-I$inc" @lib (Join-Path $root 'tests\sema_test.c') `
        -o (Join-Path $bin 'sema_test.exe')
    if ($LASTEXITCODE -ne 0) { throw '语义测试构建失败' }
    & (Join-Path $bin 'sema_test.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '语义测试未通过' }

    Write-Host '编译IR测试 ...'
    & $gcc @flags "-I$inc" @lib (Join-Path $root 'tests\ir_test.c') `
        -o (Join-Path $bin 'ir_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'IR测试构建失败' }
    & (Join-Path $bin 'ir_test.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'IR测试未通过' }

    Write-Host '编译后端/链接测试 ...'
    & $gcc @flags "-I$inc" @lib (Join-Path $root 'tests\backend_test.c') `
        -o (Join-Path $bin 'backend_test.exe')
    if ($LASTEXITCODE -ne 0) { throw '后端测试构建失败' }
    & (Join-Path $bin 'backend_test.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '后端测试未通过' }

    Write-Host '端到端：正常样例（词法） ...'
    & (Join-Path $bin 'han.exe') --dump-tokens (Join-Path $root 'tests\cases\演示.汉') |
        Out-File -Encoding utf8 (Join-Path $bin 'dump-演示.txt')
    if ($LASTEXITCODE -ne 0) { throw '正常样例词法分析失败' }

    Write-Host '端到端：正常样例（AST） ...'
    & (Join-Path $bin 'han.exe') --dump-ast (Join-Path $root 'tests\cases\演示.汉') |
        Out-File -Encoding utf8 (Join-Path $bin 'ast-演示.txt')
    if ($LASTEXITCODE -ne 0) { throw '正常样例AST失败' }

    Write-Host '端到端：正常样例（IR） ...'
    & (Join-Path $bin 'han.exe') --dump-ir (Join-Path $root 'tests\cases\演示.汉') |
        Out-File -Encoding utf8 (Join-Path $bin 'ir-演示.txt')
    if ($LASTEXITCODE -ne 0) { throw '正常样例IR失败' }

    Write-Host '端到端：错误样例（应返回 1）...'
    $errFile = Join-Path $bin '错误样例-输出.txt'
    & (Join-Path $bin 'han.exe') --dump-tokens `
        (Join-Path $root 'tests\cases\错误示例.汉') > $errFile 2>&1
    $code = $LASTEXITCODE
    Get-Content $errFile -Encoding UTF8 | Write-Host
    if ($code -ne 1) { throw "错误样例应返回 1，实际 $code" }

    Write-Host '端到端：演示样例（编译 -> 运行 -> 比对输出） ...'
    $demoExe = Join-Path $bin '演示.exe'
    & (Join-Path $bin 'han.exe') (Join-Path $root 'tests\cases\演示.汉') -o $demoExe
    if ($LASTEXITCODE -ne 0) { throw '演示样例编译失败' }
    & $demoExe | Out-File -Encoding utf8 (Join-Path $bin '演示-输出.txt')
    if ($LASTEXITCODE -ne 0) { throw '演示样例运行失败' }
    $demo = Get-Content (Join-Path $bin '演示-输出.txt') -Encoding UTF8
    if ($demo -notcontains '你好，汉语言！' -or $demo -notcontains '3') {
        throw '演示样例输出与期望不符'
    }

    Write-Host '端到端：标准库样例（编译 -> 运行 -> 比对输出） ...'
    $stdExe = Join-Path $bin '标准库.exe'
    & (Join-Path $bin 'han.exe') (Join-Path $root 'tests\cases\标准库.汉') -o $stdExe
    if ($LASTEXITCODE -ne 0) { throw '标准库样例编译失败' }
    & $stdExe | Out-File -Encoding utf8 (Join-Path $bin '标准库-输出.txt')
    if ($LASTEXITCODE -ne 0) { throw '标准库样例运行失败' }
    $want = Get-Content (Join-Path $root 'tests\cases\标准库-期望.txt') -Encoding UTF8
    $got  = Get-Content (Join-Path $bin '标准库-输出.txt') -Encoding UTF8
    $diff = Compare-Object -ReferenceObject $want -DifferenceObject $got
    if ($diff) { $diff | Format-Table -AutoSize | Out-Host; throw '标准库样例输出与期望不符' }

    Write-Host '端到端：高级特性样例（导入/默认参数/可变参数/闭包/字典，编译 -> 运行 -> 比对） ...'
    $advExe = Join-Path $bin '高级特性.exe'
    & (Join-Path $bin 'han.exe') (Join-Path $root 'tests\cases\高级特性.汉') -o $advExe
    if ($LASTEXITCODE -ne 0) { throw '高级特性样例编译失败' }
    & $advExe | Out-File -Encoding utf8 (Join-Path $bin '高级特性-输出.txt')
    if ($LASTEXITCODE -ne 0) { throw '高级特性样例运行失败' }
    $wantAdv = Get-Content (Join-Path $root 'tests\cases\高级特性-期望.txt') -Encoding UTF8
    $gotAdv  = Get-Content (Join-Path $bin '高级特性-输出.txt') -Encoding UTF8
    $diffAdv = Compare-Object -ReferenceObject $wantAdv -DifferenceObject $gotAdv
    if ($diffAdv) { $diffAdv | Format-Table -AutoSize | Out-Host; throw '高级特性样例输出与期望不符' }

    Write-Host '端到端：交互样例（输入文字/输入整数/输入数/文字转数，重定向喂入） ...'
    $ioExe = Join-Path $bin '交互.exe'
    & (Join-Path $bin 'han.exe') (Join-Path $root 'tests\cases\交互.汉') -o $ioExe
    if ($LASTEXITCODE -ne 0) { throw '交互样例编译失败' }
    # 输入/输出都走文件重定向，避免 PowerShell 管道对原生命令的编码干扰
    $ioIn  = Join-Path $bin '交互-输入.txt'
    $ioOut = Join-Path $bin '交互-输出.txt'
    [System.IO.File]::WriteAllLines($ioIn, @('小明', '21', '7'),
        (New-Object System.Text.UTF8Encoding($false)))
    $p = Start-Process -FilePath $ioExe -NoNewWindow -Wait -PassThru `
         -RedirectStandardInput $ioIn -RedirectStandardOutput $ioOut
    if ($p.ExitCode -ne 0) { throw '交互样例运行失败' }
    $wantIo = '请问你叫什么名字？', '你好，小明！', '请输入一个整数：', '它的两倍是：',
              '42', '请输入一个小数：', '它的一半是：', '3.5', '4'
    $gotIo  = Get-Content (Join-Path $bin '交互-输出.txt') -Encoding UTF8
    $diffIo = Compare-Object -ReferenceObject $wantIo -DifferenceObject $gotIo
    if ($diffIo) { $diffIo | Format-Table -AutoSize | Out-Host; throw '交互样例输出与期望不符' }
}

Write-Host "构建完成：$bin\han.exe"
exit 0
