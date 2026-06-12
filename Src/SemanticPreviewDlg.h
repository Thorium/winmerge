/**
 * @file  SemanticPreviewDlg.h
 *
 * @brief Declaration of the safe semantic copy preview dialog.
 */
#pragma once

#include "TrDialogs.h"
#include "UnicodeString.h"
#include "CCrystalTextBuffer.h"
#include "resource.h"

/**
 * @brief Preview dialog for safe semantic copy suggestions.
 *
 * Shows the correspondence summary at the top and the proposed definition
 * text in a read-only, syntax highlighted crystal editor view. Returns IDOK
 * when the user chooses to apply the suggestion.
 */
class CSemanticPreviewDlg : public CTrDialog
{
public:
	CSemanticPreviewDlg(const String& title, const String& header, const String& code,
		const String& fileExt, CWnd* pParent = nullptr);
	virtual ~CSemanticPreviewDlg();

	enum { IDD = IDD_SEMANTIC_PREVIEW_DLG };

protected:
	virtual BOOL OnInitDialog() override;
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	DECLARE_MESSAGE_MAP()

private:
	class CPreviewTextView;
	void AdjustLayout();

	String m_sTitle;
	String m_sHeader;
	String m_sCode;
	String m_sFileExt;
	CCrystalTextBuffer m_buffer;
	CPreviewTextView* m_pView; /**< child view; deletes itself in PostNcDestroy */
};
