/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// TreeItem represents an item in a TreeView control
typedef UINT_PTR TreeItem;

// TreeModel provides data to TreeCtrl
struct TreeModel {
    static const TreeItem kNullItem = 0;

    virtual ~TreeModel() = default;

    virtual TreeItem Root() = 0;

    // TODO: convert to char*
    virtual WCHAR* Text(TreeItem) = 0;
    virtual TreeItem Parent(TreeItem) = 0;
    virtual int ChildCount(TreeItem) = 0;
    virtual TreeItem ChildAt(TreeItem, int index) = 0;
    // true if this tree item should be expanded i.e. showing children
    virtual bool IsExpanded(TreeItem) = 0;
    // when showing checkboxes
    virtual bool IsChecked(TreeItem) = 0;
    virtual void SetHandle(TreeItem, HTREEITEM) = 0;
    virtual HTREEITEM GetHandle(TreeItem) = 0;
};

// function called for every item in the TreeModel
// return false to stop iteration
using TreeItemVisitor = std::function<bool(TreeModel*, TreeItem)>;

bool VisitTreeModelItems(TreeModel*, const TreeItemVisitor& visitor);
