# Toggle External Connection

Windows 트레이에서 외부 인터넷을 차단하고 지정한 인트라넷 연결은 유지하는 도구입니다. 아이콘을 한 번 누르면 전환하고, 오른쪽 클릭으로 내부망 설정과 차단 기록을 엽니다. 실행 시 관리자 권한이 필요합니다.

| 배포 파일 | 대상 |
| --- | --- |
| `ToggleExternalConnection-Win7-x64-Setup.exe` | Windows 7 SP1 x64 |
| `ToggleExternalConnection-Win10-11-x64-Setup.exe` | Windows 10·11 x64 |

[정식 릴리스와 SHA-256](https://github.com/yunhyok/toggle-external-connection/releases/latest)에서 해당 OS용 오프라인 설치 파일을 받으세요.

설치 화면에서 기본 경로인 `Program Files\Toggle External Connection`을 확인하고 다른 드라이브·폴더를 선택할 수 있습니다. 관리자 권한으로 실행하는 앱이므로 로컬 NTFS 드라이브와 보호된 상위 폴더가 필요합니다. 예를 들어 다른 드라이브 바로 아래의 새 `D:\Toggle External Connection` 폴더를 선택할 수 있습니다. 일반 사용자가 변경할 수 있는 상위 폴더, 네트워크·이동식 드라이브, 정션·심볼릭 링크는 지원하지 않습니다. 기존 상위 폴더의 권한은 변경하지 않습니다.

업데이트에서는 기존 설치 경로가 먼저 표시됩니다. 같은 위치의 업데이트는 설정·기록을 유지합니다. 다른 위치를 선택하면 새 위치에 `Data`를 복사하여 내용 일치를 확인한 뒤 이전 설치를 제거하고 설치를 진행합니다. 이전 설치 폴더와 그 하위·상위가 겹치는 위치는 선택하지 마세요. 이동에 실패하면 이전 `Data` 또는 새 위치의 검증된 복사본을 보존하며, 오류 화면에서 복구 방법을 안내합니다. 제거를 직접 실행하면 설정·기록도 삭제됩니다.

### 처음 실행할 때

1. **인트라넷을 사용하는 어댑터**를 선택합니다. 현재 서브넷과 DNS 서버는 후보로 채워집니다. 다른 서브넷에 있는 파일 서버나 업무 서버는 IP, CIDR, IPv4 패턴으로 추가합니다.
2. 목록을 확인하고 저장합니다. 차단 중에는 선택한 어댑터에서만 이 주소들을 허용합니다. 별도 DNS 예외는 해당 서버의 TCP·UDP 53에 적용됩니다. 허용한 네트워크 대역 안에 있는 서버는 그 대역 규칙을 따릅니다.
3. 처음 시작할 때는 허용 상태입니다. 트레이 아이콘을 한 번 누르면 차단하고, 다시 누르면 해제합니다. 창을 닫으면 트레이에 남고, 트레이에서 종료하거나 프로세스가 끝나면 Windows가 앱의 규칙을 회수합니다.

내부 프록시나 원격 데스크톱을 허용하면 그 서버에서 다시 접속하는 외부 사이트까지 이 PC에서 구분할 수 없습니다. 규칙은 로컬 Windows 호스트의 IPv4·IPv6 송수신에 적용됩니다. 독립적인 가상 머신이나 별도 장비의 경유 트래픽은 보호 대상에 포함되지 않습니다.

### 01 / 허용망 입력 — IPv4 패턴

| 입력 | 허용 범위 |
|:---:|:---:|
| `172.16.1.*` | `172.16.1.0` ~ `172.16.1.255` |
| `172.16.*.*` | `172.16.0.0/16` |
| `172.16.1.2?` | `172.16.1.20` ~ `172.16.1.29` |

한 줄에 한 범위를 입력할 것. `*`는 한 옥텟 안에서 숫자 0개 이상, `?`는 숫자 한 자리. 모든 IPv4 주소를 허용하는 패턴과 512개 초과 CIDR로 펼쳐지는 목록은 저장할 수 없음. IPv6는 개별 IP 또는 CIDR, DNS는 개별 IP로 입력.

### 02 / 개발 앱 — 차단 중 외부 접속 예외

기본값은 모두 꺼짐. 사용할 앱을 먼저 열고, **설정 → 차단 중 외부 접속을 허용할 개발 앱**에서 필요한 항목을 체크한 뒤 저장할 것. 해제는 해당 체크를 끄고 저장. 차단 중 설정을 바꾸려면 먼저 차단을 해제할 것.

| 선택 | 허용되는 연결 |
|:---:|:---:|
| Codex | 설치된 Codex 데스크톱과 실행 중인 버전별 Codex 통신 프로세스 |
| Claude | 설치된 Windows 패키지의 Claude 데스크톱 |
| Antigravity | 기본 사용자 설치의 Antigravity·Antigravity IDE와 전용 언어 서버 |
| Git · gh | Git for Windows, Git의 HTTPS·SSH·LFS 도구, GitHub CLI, 탐지된 GitHub Desktop |
| GitHub 웹 | 트레이의 **MiniBrowser에서 GitHub 열기** 또는 시작 메뉴의 **GitHub Web** |

앱 예외는 실행 파일의 **전체 경로**를 기준으로 모든 목적지에 적용. 동일 IP를 사용하는 다른 앱은 계속 차단. Git에 기본 포함된 SSH는 허용되며, 별도 시스템 SSH·터미널·범용 셸까지 허용 범위가 확장되지는 않음. 업데이트로 실행 파일이 교체되면 앱을 연 뒤 차단을 다시 전환할 것. 지명된 경로의 실행 파일은 신뢰할 수 있는 설치본으로 유지할 것.

GitHub 전용 창은 현재 사용자에게 등록된 [MiniBrowser 0.2.4 설치본](https://github.com/yunhyok/MiniBrowser/releases/tag/v0.2.4)을 사용. 일반 MiniBrowser 창은 계속 사용할 수 있으며, 전용 창에서 처음 한 번 별도로 로그인할 것. MiniBrowser 미설치 시 설치 안내 표시. GitHub, 정적 파일, 사용자 콘텐츠 도메인(`github.com`, `githubassets.com`, `githubusercontent.com` 및 그 하위 도메인)의 HTTPS만 중계. 외부 SSO·GitHub Pages·임의 외부 링크는 별도 사이트이므로 제한. 업무에 쓰는 DNS 서버는 허용 목록에서 확인할 것.

**Google 계정으로 GitHub 로그인**이 필요하면 `GitHub 웹`과 `GitHub 웹에서 Google 로그인 허용`을 함께 체크하고 저장할 것. 기본값은 꺼짐. 켠 경우 전용 MiniBrowser의 HTTPS 중계에 정확한 호스트 여섯 개만 추가: `accounts.google.com`, `accounts.gstatic.com`, `ssl.gstatic.com`, `www.gstatic.com`, `fonts.googleapis.com`, `fonts.gstatic.com`. Google 검색·Gmail·Drive 제외. 터널 연결 오류는 주소 허용 여부를, Google이 표시하는 브라우저 제한 오류는 [Google OAuth 안내](https://developers.google.com/identity/protocols/oauth2/web-server#authorization-errors)를 확인할 것.

### 차단 기록

**차단 기록 보기**에서 프로그램 경로와 목적지 IP로 검색할 수 있습니다. 기록에는 처음·마지막 관측 시각, 관측 횟수, 실행 파일, 목적지 IP·포트, 프로토콜이 표시됩니다. 같은 경로·IP·포트·프로토콜로 첫 관측부터 60초 이내에 일어난 차단을 묶습니다. 이름 단서는 PC의 DNS 역방향 캐시에서만 읽어 실제 목적지 IP와 별도로 표시합니다.

이 앱이 차단 중 거부한 새 외부 송신 시도만 기록합니다. Windows에서 앱 ID를 제공하지 않으면 프로그램 미확인으로 표시합니다. 수집·저장에 문제가 생기면 차단 상태와 기록 오류를 따로 표시합니다. 실제 업무 주소와 기록은 설치된 PC의 보호된 `Data` 폴더에만 저장합니다. 앱을 실행하면 오늘을 포함해 7일간의 기록을 남기고 만료분을 정리합니다. 제거 시 설정과 차단 기록도 삭제됩니다.

### 빌드와 검증

Visual Studio 2022 C++ x64 도구(v143), Windows SDK 및 Inno Setup 6.5 이상 또는 7이 필요합니다. **x64 Native Tools Command Prompt**에서 PowerShell을 시작한 뒤 저장소 루트에서 실행합니다.

```powershell
foreach ($target in 'Win7','Win10-11') {
    .\scripts\build.ps1 -Target $target
    if (!$?) { throw "Build failed" }
    ISCC.exe "/DTarget=$target" packaging\setup.iss
    if ($LASTEXITCODE) { throw "Installer failed" }
}
```

빌드 스크립트는 정적 런타임으로 실행 파일·설치 경로 검사 도구를 생성하고 오프라인 테스트를 실행합니다. 설치 경로는 Windows의 64비트 관리자 전용 설치 등록에서 읽으며 실행 파일과 상위 경로의 권한·링크 여부를 확인한 뒤에만 권한 상승을 요청합니다. `dist`에 두 설치 파일이 출력됩니다. 설치하지 않고 화면만 점검하려면 빌드한 실행 파일에 `--preview` 또는 `--preview-history`를 붙이세요. 이 모드는 예시 데이터만 사용하며 관리자 권한이 필요 없습니다.

격리된 시험 절차는 [별도 PC 검증 안내](docs/TESTING.md)에 있습니다. 빌드와 정적 테스트만으로 OS별 실제 차단·복구 성공을 판정하지 않습니다.
