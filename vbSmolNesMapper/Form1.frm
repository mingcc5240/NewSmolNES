VERSION 5.00
Begin VB.Form Form1 
   BorderStyle     =   1  '단일 고정
   Caption         =   "VbSmolNES Sound -made by minGC"
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
' Form1 상단 선언부에 추가
Private m_GameTitle As String
Private m_MapperNum As Long


' ========================================================
' 60 FPS 고정 에뮬레이션 메인 루프
' ========================================================
Private Sub RunGameLoop()
    Dim nextFrameTime As Double
    Dim currentTime As Double
    Const FRAME_DELTA As Double = 1000# / 60# ' 약 16.666 ms

    Dim fpsTimer As Long
    Dim frameCounter As Long
    Dim currentFPS As Long
    Dim fpsText As String

    Call SetBkMode(picScreen.hdc, TRANSPARENT)

    nextFrameTime = timeGetTime()
    fpsTimer = timeGetTime()
    frameCounter = 0
    currentFPS = 60

    Do While m_Running
        currentTime = timeGetTime()

        If currentTime >= nextFrameTime Then
            ProcessInput

            ' NES 1프레임 연산
            ' (내부에서 화면 연산과 함께 735개의 44.1kHz 사운드 샘플이 waveOut으로 스트리밍됨)
            NES_RunFrame

            'CopyMemory m_pBits, m_pNESBuffer, 245760
            CopyMemory m_pBits, m_pNESBuffer, 983040
            
            ' 3. StretchBlt로 소스(512x480) -> 대상(768x720)으로 3배 확대 출력
            StretchBlt picScreen.hdc, 0, 0, NES_WIDTH * SCALE_FACTOR, NES_HEIGHT * SCALE_FACTOR, _
                       m_hMemDC, 0, 0, 512, 480, SRCCOPY
              
            'BitBlt picScreen.hdc, 0, 0, 512, 480, m_hMemDC, 0, 0, SRCCOPY
            
            ' FPS 카운트 및 표시
            frameCounter = frameCounter + 1
            If (currentTime - fpsTimer) >= 1000 Then
                currentFPS = frameCounter
                frameCounter = 0
                fpsTimer = currentTime
               ' Me.Caption = "VbSmolNES - " & currentFPS & " FPS"
                Me.Caption = "MGC VbSmolNES - [" & m_GameTitle & "] (Mapper " & m_MapperNum & ") - " & currentFPS & " FPS"
            End If

            fpsText = "FPS: " & CStr(currentFPS)
            Call SetTextColor(picScreen.hdc, &H0)
            TextOut picScreen.hdc, 9, 9, fpsText, Len(fpsText)
            Call SetTextColor(picScreen.hdc, &HFFFF&)
            TextOut picScreen.hdc, 8, 8, fpsText, Len(fpsText)

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

Private Sub ProcessInput()
    Dim k As Byte
    k = 0

    If (GetAsyncKeyState(vbKeyX) And &H8000) Then k = k Or &H1
    If (GetAsyncKeyState(vbKeyZ) And &H8000) Then k = k Or &H2
    If (GetAsyncKeyState(vbKeyTab) And &H8000) Then k = k Or &H4
    If (GetAsyncKeyState(vbKeyReturn) And &H8000) Then k = k Or &H8
    If (GetAsyncKeyState(vbKeyUp) And &H8000) Then k = k Or &H10
    If (GetAsyncKeyState(vbKeyDown) And &H8000) Then k = k Or &H20
    If (GetAsyncKeyState(vbKeyLeft) And &H8000) Then k = k Or &H40
    If (GetAsyncKeyState(vbKeyRight) And &H8000) Then k = k Or &H80

    NES_SetInput k
End Sub

Private Sub Form_QueryUnload(Cancel As Integer, UnloadMode As Integer)
    m_Running = False

    ' 사운드 장치 해제 (추가된 핵심 호출)
    Call NES_CloseAudio

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
        .biWidth = 512
        .biHeight = -480
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
   
   ' m_Running = False

    ' 사운드 장치 해제 (추가된 핵심 호출)
   ' Call NES_CloseAudio
    
   romPath = SelectNESRom(Me.hWnd)
    If Len(romPath) = 0 Then
        Unload Me
        Exit Sub
    End If

    ' 파일명 및 매퍼 번호 읽기
    m_GameTitle = GetGameTitle(romPath)
    m_MapperNum = GetNESMapperNumber(romPath)
    
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

    ' 5. 사운드 시스템 초기화 (추가된 핵심 호출)
    Call NES_InitAudio
    
    Call timeBeginPeriod(1)
    m_Running = True

    RunGameLoop
End Sub
