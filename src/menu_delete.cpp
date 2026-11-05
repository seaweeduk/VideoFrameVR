#include "menu_style.h"

#include "menu_stash.h"  // stashPrinted
#include "stash_stream.h"

// The delete dialog (docs/alpha/delete-file.md): opened by the strip's
// button with what the menu knows at once, filled in once the app has
// looked the file up in the background, then "Deleting …" until the
// deletion ends.

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

void Menu::layoutDeleteDialog(std::vector<Button>& row) {
  const Strings& t = text();
  const bool deleting = dialog_ == Dialog::Deleting;
  Button cancel = textButton(t.cancel, Icon::None, [this] {
    dialog_ = Dialog::None;
    MenuCommand c;
    c.type = MenuCommand::CancelDelete;
    return c;
  });
  cancel.disabled = deleting;
  row.push_back(cancel);
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
  const DeleteInfo& info = deleteInfo_;
  if (!info.source.empty()) {
    Button copy = textButton(t.deletion.removeDownload, Icon::None, confirm(true));
    copy.disabled = deleting || !info.ready;
    row.push_back(copy);
  }
  Button go = textButton(info.source.empty() ? t.deletion.deleteButton : t.deletion.everywhere,
                         Icon::Trash, confirm(false));
  go.danger = true;
  go.disabled = deleting || !info.ready || (!info.source.empty() && !info.sourceFound);
  row.push_back(go);
}

void Menu::drawDeleteDialog(cairo_t* cr, double textX, double boxY) {
  const Strings& t = text();
  const DeleteStrings& d = t.deletion;
  const DeleteInfo& info = deleteInfo_;
  const double textW = kDialogW - 100;
  drawText(cr, info.photo ? t.deletePhotoTitle : t.deleteTitle, textX, boxY + 30, textW, 60, 38,
           true, true, 1, 1, 1);
  const std::string name = info.ready ? info.name + " · " + sizeText(info.size) : info.name;
  drawText(cr, name, textX, boxY + 95, textW, 50, 28, false, true, 0.85, 0.85, 0.9);
  const std::string& at = info.source.empty() ? info.file : info.source;
  std::string where = isStashScene(at) || isStashSource(at) ? std::string(d.inStash)
                      : remote::isRemote(at) ? stashPrinted(d.onServer, serverName(remote::root(at)))
                                       : std::string(d.onDevice);
  if (!info.source.empty())
    where += " · " + stashPrinted(t.offline.tipDownloaded, driveName(info.copyRoot, false));
  drawText(cr, where, textX, boxY + 150, textW, 44, 26, false, true, 0.7, 0.7, 0.75);
  double y = boxY + 205;
  if (info.sourceChanged) {
    drawText(cr, d.sourceChanged, textX, y, textW, 44, 26, false, true, 0.7, 0.7, 0.75);
    y += 55;
  } else if (info.stash != DeleteInfo::Stash::None) {
    const std::string stash = info.stash == DeleteInfo::Stash::Lists    ? std::string(d.stashLists)
                              : info.stash == DeleteInfo::Stash::Unsure ? std::string(d.stashUnsure)
                              : info.otherFiles == 0                    ? std::string(d.stashAlso)
                              : info.otherFiles == 1 ? std::string(d.stashKeepsOther)
                                                     : stashPrinted(d.stashKeepsOthers, info.otherFiles);
    drawText(cr, stash, textX, y, textW, 44, 26, false, true, 0.7, 0.7, 0.75);
    y += 55;
  }
  if (dialog_ == Dialog::Deleting)
    drawText(cr, d.deleting, textX, y, textW, 44, 26, true, true, 0.85, 0.85, 0.9);
  else
    drawText(cr, info.stash == DeleteInfo::Stash::Deletes && info.stashTrash ? d.stashTrash
                                                                              : d.forGood,
             textX, y, textW, 44, 26, true, true, 1.0, 0.45, 0.4);
}
