VERSION 5.00
Begin VB.Form Form1 
   BorderStyle     =   1  '단일 고정
   Caption         =   "VbSmolNES -made by minGC"
   ClientHeight    =   7155
   ClientLeft      =   150
   ClientTop       =   795
   ClientWidth     =   9255
   LinkTopic       =   "Form1"
   MaxButton       =   0   'False
   MinButton       =   0   'False
   ScaleHeight     =   477
   ScaleMode       =   3  '픽셀
   ScaleWidth      =   617
   StartUpPosition =   3  'Windows 기본값
   Begin VB.PictureBox picScreen 
      Height          =   3600
      Left            =   120
      ScaleHeight     =   236
      ScaleMode       =   3  '픽셀
      ScaleWidth      =   252
      TabIndex        =   0
      Top             =   120
      Width           =   3840
   End
   Begin VB.Menu mFile 
      Caption         =   "File"
      Begin VB.Menu mLoad 
         Caption         =   "Load"
      End
      Begin VB.Menu mExit 
         Caption         =   "Exit"
      End
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

Private Const NES_WIDTH As Long = 256
Private Const NES_HEIGHT As Long = 240
Private Const SCALE_FACTOR As Long = 3

Private m_hMemDC As Long        ' 가상 메모리 DC
Private m_hDIB As Long          ' DIB 비트맵 핸들
Private m_pBits As Long         ' DIB 실제 픽셀 메모리 시작 포인터
Private m_pNESBuffer As Long    ' DLL 내부 screen_buffer 포인터
Private m_Running As Boolean    ' 에뮬레이션 루프 동작 플래그

Private Sub Form_Load()
     Dim bi As BITMAPINFO


    ' 고정폭 굵은 글꼴로 설정
    picScreen.Font.Name = "Consolas"  ' 또는 "Courier New", "Tahoma"
    picScreen.Font.Size = 11
    picScreen.Font.Bold = True
    
    ' 1. PictureBox 크기를 2배(512 x 480)로 설정
    picScreen.AutoRedraw = False
    picScreen.ScaleMode = vbPixels
    picScreen.Width = NES_WIDTH * SCALE_FACTOR   ' 512
    picScreen.Height = NES_HEIGHT * SCALE_FACTOR ' 480
    picScreen.Left = 0
    picScreen.Top = 0

    ' 폼 자체의 크기를 PictureBox에 맞게 자동 조정
    Me.ScaleMode = vbPixels
    Me.Width = (picScreen.Width + (Me.Width / Screen.TwipsPerPixelX - Me.ScaleWidth)) * Screen.TwipsPerPixelX
    Me.Height = (picScreen.Height + (Me.Height / Screen.TwipsPerPixelY - Me.ScaleHeight)) * Screen.TwipsPerPixelY

    ' 2. 픽셀 보간 없이 선명한 도트 그래픽을 유지하도록 설정
    Call SetStretchBltMode(picScreen.hdc, COLORONCOLOR)

    ' 3. 원본 256x240 크기의 DIB Section 생성 (Top-down)
    With bi.bmiHeader
        .biSize = Len(bi.bmiHeader)
        .biWidth = NES_WIDTH
        .biHeight = -NES_HEIGHT
        .biPlanes = 1
        .biBitCount = 32
        .biCompression = 0
    End With

    m_hMemDC = CreateCompatibleDC(0)
    m_hDIB = CreateDIBSection(m_hMemDC, bi, DIB_RGB_COLORS, m_pBits, 0, 0)
    Call SelectObject(m_hMemDC, m_hDIB)

    ' 4. 파일 대화상자로 ROM 파일 선택
    Me.Show
    DoEvents
    
   
End Sub

' ========================================================
' 60 FPS 고정 에뮬레이션 메인 루프
' ========================================================

Private Sub RunGameLoop()
    Dim nextFrameTime As Double
    Dim currentTime As Double
    Const FRAME_DELTA As Double = 1000# / 60# ' 약 16.666 ms

    ' FPS 측정용 변수
    Dim fpsTimer As Long
    Dim frameCounter As Long
    Dim currentFPS As Long
    Dim fpsText As String

    ' 텍스트 투명 배경 및 글자색 설정 (노란색: &H00FFFF, 흰색: &HFFFFFF)
    Call SetBkMode(picScreen.hdc, TRANSPARENT)
    Call SetTextColor(picScreen.hdc, &HFFFF)

    nextFrameTime = timeGetTime()
    fpsTimer = timeGetTime()
    frameCounter = 0
    currentFPS = 60

    Do While m_Running
        currentTime = timeGetTime()

        If currentTime >= nextFrameTime Then
            ' 1. 입력 처리
            ProcessInput

            ' 2. 1프레임 연산
            NES_RunFrame

            ' 3. 메모리 복사 및 확대 렌더링
            CopyMemory m_pBits, m_pNESBuffer, 245760
            StretchBlt picScreen.hdc, 0, 0, NES_WIDTH * SCALE_FACTOR, NES_HEIGHT * SCALE_FACTOR, _
                       m_hMemDC, 0, 0, NES_WIDTH, NES_HEIGHT, SRCCOPY

            ' 4. 프레임 카운트 증가
            frameCounter = frameCounter + 1

            ' 5. 1초(1000ms) 경과 시 FPS 갱신
            If (currentTime - fpsTimer) >= 1000 Then
                currentFPS = frameCounter
                frameCounter = 0
                fpsTimer = currentTime
                
                ' [방법 1] 폼 캡션에 FPS 출력
                Me.Caption = "VbSmolNES - " & currentFPS & " FPS"
            End If

            ' 6. [방법 2] 화면 좌측 상단에 직접 텍스트 오버레이 출력 (x=8, y=8)
            ' 기존 TextOut 대신 그림자(검정) + 본문(밝은 노랑/라임색) 2중 출력
            fpsText = "FPS: " & CStr(currentFPS)
            
            ' 1) 그림자 효과 (검은색으로 상하좌우 오프셋 출력)
            Call SetTextColor(picScreen.hdc, &H0)
            TextOut picScreen.hdc, 9, 9, fpsText, Len(fpsText)
            
            ' 2) 본문 텍스트 (밝은 연두색 또는 형광 노란색)
            Call SetTextColor(picScreen.hdc, &HFFFF&)   ' 노란색 (&H00FF00& 은 라임색)
            TextOut picScreen.hdc, 8, 8, fpsText, Len(fpsText)

            ' 다음 프레임 기준 시각 갱신
            nextFrameTime = nextFrameTime + FRAME_DELTA
            If (currentTime - nextFrameTime) > (FRAME_DELTA * 2) Then
                nextFrameTime = currentTime + FRAME_DELTA
            End If
        Else
            If (nextFrameTime - currentTime) > 2 Then
                Sleep 1
            End If
        End If

        DoEvents
    Loop
End Sub
' ========================================================
' 키패드 입력 수신 (smolnes $4016 시프트 순서에 맞춤)
' 1바이트 비트 배치:
' Bit 0: A
' Bit 1: B
' Bit 2: Select
' Bit 3: Start
' Bit 4: Up
' Bit 5: Down
' Bit 6: Left
' Bit 7: Right
' ========================================================
Private Sub ProcessInput()
    Dim k As Byte
    k = 0

    ' X 키: A 버튼
    If (GetAsyncKeyState(vbKeyX) And &H8000) Then k = k Or &H1
    ' Z 키: B 버튼
    If (GetAsyncKeyState(vbKeyZ) And &H8000) Then k = k Or &H2
    ' Tab 키: Select
    If (GetAsyncKeyState(vbKeyTab) And &H8000) Then k = k Or &H4
    ' Enter 키: Start
    If (GetAsyncKeyState(vbKeyReturn) And &H8000) Then k = k Or &H8
    ' 방향키 (상, 하, 좌, 우)
    If (GetAsyncKeyState(vbKeyUp) And &H8000) Then k = k Or &H10
    If (GetAsyncKeyState(vbKeyDown) And &H8000) Then k = k Or &H20
    If (GetAsyncKeyState(vbKeyLeft) And &H8000) Then k = k Or &H40
    If (GetAsyncKeyState(vbKeyRight) And &H8000) Then k = k Or &H80

    NES_SetInput k
End Sub

' ========================================================
' 종료 시 리소스 해제
' ========================================================
Private Sub Form_QueryUnload(Cancel As Integer, UnloadMode As Integer)
    m_Running = False
    Call timeEndPeriod(1)

    If m_hDIB <> 0 Then
        Call DeleteObject(m_hDIB)
        m_hDIB = 0
    End If
    If m_hMemDC <> 0 Then
        Call DeleteDC(m_hMemDC)
        m_hMemDC = 0
    End If
End Sub

Private Sub mExit_Click()
  End
End Sub

Private Sub mLoad_Click()
   Dim romPath As String
   
   romPath = SelectNESRom(Me.hWnd)
    If Len(romPath) = 0 Then
        Unload Me
        Exit Sub
    End If

    ' 5. 선택한 ROM 로드
    If NES_LoadROM(romPath) = 0 Then
        MsgBox "ROM 로드 실패: " & romPath, vbCritical
        Unload Me
        Exit Sub
    End If

    m_pNESBuffer = NES_GetBuffer()
    If m_pNESBuffer = 0 Then
        MsgBox "버퍼 포인터 획득 실패", vbCritical
        Unload Me
        Exit Sub
    End If

    Call timeBeginPeriod(1)
    m_Running = True

    RunGameLoop
End Sub
