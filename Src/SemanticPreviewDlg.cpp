/**
 * @file  SemanticPreviewDlg.cpp
 *
 * @brief Implementation of the safe semantic copy preview dialog.
 */
#include "StdAfx.h"
#include "SemanticPreviewDlg.h"
#include "Merge.h"
#include "DarkModeLib.h"
#include "CCrystalTextView.h"
#include "crystalparser.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

/**
 * @brief Read-only crystal view over the dialog-owned preview buffer.
 */
class CSemanticPreviewDlg::CPreviewTextView : public CCrystalTextView
{
public:
	explicit CPreviewTextView(CCrystalTextBuffer* pBuffer) : m_pBuffer(pBuffer)
	{
		SetParser(&m_xParser);
	}

	virtual CCrystalTextBuffer* LocateTextBuffer() override { return m_pBuffer; }

	virtual void OnInitialUpdate() override
	{
		// This view has no CDocument, so CCrystalTextView::OnInitialUpdate
		// must be skipped: it dereferences GetDocument() for the path name.
		CView::OnInitialUpdate();
		SetRevisionMarkWidth(0);
		SetSelectionMargin(false);
		SetFont(theApp.m_lfDiff);
		AttachToBuffer(nullptr);
		SetColorContext(theApp.GetMainSyntaxColors());
		if (HWND hSelf = GetSafeHwnd())
			DarkMode::setDarkScrollBar(hSelf);
	}

private:
	CCrystalParser m_xParser;
	CCrystalTextBuffer* m_pBuffer;
};

CSemanticPreviewDlg::CSemanticPreviewDlg(const String& title, const String& header, const String& code,
	const String& fileExt, CWnd* pParent /*= nullptr*/)
	: CTrDialog(CSemanticPreviewDlg::IDD, pParent)
	, m_sTitle(title)
	, m_sHeader(header)
	, m_sCode(code)
	, m_sFileExt(fileExt)
	, m_pView(nullptr)
{
}

CSemanticPreviewDlg::~CSemanticPreviewDlg()
{
}

BEGIN_MESSAGE_MAP(CSemanticPreviewDlg, CTrDialog)
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
END_MESSAGE_MAP()

BOOL CSemanticPreviewDlg::OnInitDialog()
{
	CTrDialog::OnInitDialog();

	if (!m_sTitle.empty())
		SetWindowText(m_sTitle.c_str());
	SetDlgItemText(IDC_SEMANTIC_PREVIEW_HEADER, m_sHeader);

	m_buffer.InitNew();
	if (!m_sCode.empty())
	{
		int nEndLine = 0;
		int nEndChar = 0;
		m_buffer.InsertText(nullptr, 0, 0, m_sCode.c_str(), m_sCode.length(), nEndLine, nEndChar, 0, false);
	}
	m_buffer.SetReadOnly(true);

	CWnd* pPlaceholder = GetDlgItem(IDC_SEMANTIC_PREVIEW_CODE);
	CRect rc;
	pPlaceholder->GetWindowRect(&rc);
	ScreenToClient(&rc);
	pPlaceholder->DestroyWindow();

	m_pView = new CPreviewTextView(&m_buffer);
	m_pView->Create(nullptr, nullptr, AFX_WS_DEFAULT_VIEW, rc, this, IDC_SEMANTIC_PREVIEW_CODE, nullptr);
	m_pView->SendMessage(WM_INITIALUPDATE);
	if (!m_sFileExt.empty())
		m_pView->SetTextType(m_sFileExt.c_str());

	AdjustLayout();
	return TRUE;
}

void CSemanticPreviewDlg::OnSize(UINT nType, int cx, int cy)
{
	CTrDialog::OnSize(nType, cx, cy);
	AdjustLayout();
}

void CSemanticPreviewDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	lpMMI->ptMinTrackSize.x = 360;
	lpMMI->ptMinTrackSize.y = 240;
	CTrDialog::OnGetMinMaxInfo(lpMMI);
}

void CSemanticPreviewDlg::AdjustLayout()
{
	CWnd* pHeader = GetDlgItem(IDC_SEMANTIC_PREVIEW_HEADER);
	CWnd* pOk = GetDlgItem(IDOK);
	CWnd* pCancel = GetDlgItem(IDCANCEL);
	if (pHeader == nullptr || pOk == nullptr || pCancel == nullptr ||
		m_pView == nullptr || m_pView->m_hWnd == nullptr)
		return;

	CRect rcClient;
	GetClientRect(&rcClient);
	const int margin = 8;
	const int gap = 6;

	CRect rcButton;
	pOk->GetWindowRect(&rcButton);
	const int buttonWidth = rcButton.Width();
	const int buttonHeight = rcButton.Height();
	const int buttonTop = rcClient.bottom - margin - buttonHeight;
	pCancel->SetWindowPos(nullptr, rcClient.right - margin - buttonWidth, buttonTop,
		0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	pOk->SetWindowPos(nullptr, rcClient.right - margin - buttonWidth * 2 - gap, buttonTop,
		0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

	CRect rcHeader;
	pHeader->GetWindowRect(&rcHeader);
	ScreenToClient(&rcHeader);
	pHeader->MoveWindow(margin, rcHeader.top, rcClient.Width() - margin * 2, rcHeader.Height());

	const int codeTop = rcHeader.top + rcHeader.Height() + gap;
	m_pView->MoveWindow(margin, codeTop, rcClient.Width() - margin * 2, buttonTop - gap - codeTop);
}
