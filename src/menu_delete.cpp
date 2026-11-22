#include "menu_parts.h"

#include "menu_stash.h"  // stashPrinted
#include "stash_stream.h"

// The delete dialog (docs/alpha/delete-file.md): opened by the strip's
// button with what the menu knows at once, filled in once the app has
// looked the file up in the background, then "Deleting …" until the
// deletion ends.

bool Menu::canRequestDelete(const std::string& file) const {
  return view_.deletable == MenuView::Deletable::Yes && !view_.currentFile.empty() && file == view_.currentFile;
}

MenuCommand Menu::requestDelete() {
  if (!canRequestDelete(view_.currentFile) || dialog_ != Dialog::None) return {};
  deleteInfo_ = {};
  deleteInfo_.file = view_.currentFile;
  deleteInfo_.name = view_.currentFile.substr(view_.currentFile.find_last_of('/') + 1);
  deleteInfo_.photo = view_.photo;
  openDialog(Dialog::ConfirmDelete, view_.currentFile);
  MenuCommand c;
  c.type = MenuCommand::RequestDelete;
  c.path = view_.currentFile;
  return c;
}

void Menu::askDelete(const DeleteInfo& info) {
  if (dialog_ != Dialog::ConfirmDelete || dialogPath_ != info.file) return;
  deleteInfo_ = info;
  dirty_ = true;
}

void Menu::cancelDelete() {
  if (dialog_ != Dialog::ConfirmDelete) return;
  dialog_ = Dialog::None;
  dirty_ = true;
}

void Menu::showDeleteError(const std::string& error) { openDialog(Dialog::Error, "", error); }

void Menu::deleteEnded(const std::string& error, const std::string& note) {
  showPlayedFrom();
  if (!error.empty())
    showDeleteError(error);
  else if (!note.empty())
    openDialog(Dialog::Deleted, "", note);
}

// The video (or photo), where it is, what Stash does with it, and that it
// goes for good; Cancel, Remove download for a downloaded copy, and Delete.
void Menu::layoutDeleteDialog(DialogLayout& dialog) {
  using Ink = DialogLayout::Ink;
  const Strings& t = text();
  const DeleteStrings& d = t.deletion;
  const DeleteInfo& info = deleteInfo_;
  const bool deleting = dialog_ == Dialog::Deleting;
  dialog.title(info.photo ? t.deletePhotoTitle : t.deleteTitle);
  dialog.line(info.ready ? info.name + " · " + sizeText(info.size) : info.name, Ink::Strong, 2);
  const std::string& at = info.source.empty() ? info.file : info.source;
  std::string where = isStashScene(at) || isStashSource(at) ? std::string(d.inStash)
                      : remote::isRemote(at) ? stashPrinted(d.onServer, serverName(remote::root(at)))
                                             : std::string(d.onDevice);
  if (!info.source.empty())
    where += " · " + stashPrinted(t.offline.tipDownloaded, driveName(info.copyRoot, false));
  dialog.line(where);
  if (info.sourceChanged) {
    dialog.line(d.sourceChanged);
  } else if (info.stash != DeleteInfo::Stash::None) {
    dialog.line(info.stash == DeleteInfo::Stash::Lists    ? std::string(d.stashLists)
                : info.stash == DeleteInfo::Stash::Unsure ? std::string(d.stashUnsure)
                : info.otherFiles == 0                    ? std::string(d.stashAlso)
                : info.otherFiles == 1 ? std::string(d.stashKeepsOther)
                                       : stashPrinted(d.stashKeepsOthers, info.otherFiles));
  }
  if (deleting)
    dialog.line(d.deleting, Ink::Strong, 1, true);
  else
    dialog.line(info.stash == DeleteInfo::Stash::Deletes && info.stashTrash ? d.stashTrash : d.forGood,
                Ink::Warn);

  Button cancel = textButton(t.cancel, Icon::None, [this] {
    dialog_ = Dialog::None;
    MenuCommand c;
    c.type = MenuCommand::CancelDelete;
    return c;
  });
  cancel.disabled = deleting;
  dialog.button(cancel);
  // DeleteFile's value: 1 deletes only the downloaded copy.
  auto confirm = [this](bool copy) {
    return [this, copy] {
      dialog_ = Dialog::Deleting;
      MenuCommand c;
      c.type = MenuCommand::DeleteFile;
      c.path = dialogPath_;
      c.value = copy ? 1 : 0;
      return c;
    };
  };
  if (!info.source.empty()) {
    Button copy = textButton(d.removeDownload, Icon::None, confirm(true));
    copy.disabled = deleting || !info.ready;
    dialog.button(copy);
  }
  Button go = textButton(info.source.empty() ? d.deleteButton : d.everywhere, Icon::Trash,
                         confirm(false));
  go.danger = true;
  go.disabled = deleting || !info.ready || (!info.source.empty() && !info.sourceFound);
  dialog.button(go);
}
