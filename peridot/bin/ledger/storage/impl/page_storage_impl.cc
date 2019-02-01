// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/storage/impl/page_storage_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>

#include <lib/callback/trace_callback.h>
#include <lib/callback/waiter.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/arraysize.h>
#include <lib/fxl/files/directory.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/files/file_descriptor.h>
#include <lib/fxl/files/path.h>
#include <lib/fxl/files/unique_fd.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/memory/ref_ptr.h>
#include <lib/fxl/memory/weak_ptr.h>
#include <lib/fxl/strings/concatenate.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <trace/event.h>

#include "peridot/bin/ledger/cobalt/cobalt.h"
#include "peridot/bin/ledger/coroutine/coroutine_waiter.h"
#include "peridot/bin/ledger/lock/lock.h"
#include "peridot/bin/ledger/storage/impl/btree/diff.h"
#include "peridot/bin/ledger/storage/impl/btree/iterator.h"
#include "peridot/bin/ledger/storage/impl/commit_impl.h"
#include "peridot/bin/ledger/storage/impl/constants.h"
#include "peridot/bin/ledger/storage/impl/file_index.h"
#include "peridot/bin/ledger/storage/impl/file_index_generated.h"
#include "peridot/bin/ledger/storage/impl/journal_impl.h"
#include "peridot/bin/ledger/storage/impl/object_digest.h"
#include "peridot/bin/ledger/storage/impl/object_identifier_encoding.h"
#include "peridot/bin/ledger/storage/impl/object_impl.h"
#include "peridot/bin/ledger/storage/impl/split.h"
#include "peridot/bin/ledger/storage/public/constants.h"

namespace storage {

using coroutine::CoroutineHandler;

namespace {

struct StringPointerComparator {
  using is_transparent = std::true_type;

  bool operator()(const std::string* str1, const std::string* str2) const {
    return *str1 < *str2;
  }

  bool operator()(const std::string* str1, const CommitIdView* str2) const {
    return *str1 < *str2;
  }

  bool operator()(const CommitIdView* str1, const std::string* str2) const {
    return *str1 < *str2;
  }
};

size_t GetObjectPartStart(int64_t offset, size_t object_size) {
  size_t start = object_size;
  // Valid indices are between -N and N-1.
  if (offset >= -static_cast<int64_t>(object_size) &&
      offset < static_cast<int64_t>(object_size)) {
    start = offset < 0 ? object_size + offset : offset;
  }
  return start;
}

size_t GetObjectPartLength(int64_t max_size, size_t object_size, size_t start) {
  size_t adjusted_max_size = max_size < 0 ? object_size : max_size;
  return start > object_size ? 0
                             : std::min(adjusted_max_size, object_size - start);
}

}  // namespace

PageStorageImpl::PageStorageImpl(
    ledger::Environment* environment,
    encryption::EncryptionService* encryption_service, std::unique_ptr<Db> db,
    PageId page_id)
    : PageStorageImpl(environment, encryption_service,
                      std::make_unique<PageDbImpl>(environment, std::move(db)),
                      std::move(page_id)) {}

PageStorageImpl::PageStorageImpl(
    ledger::Environment* environment,
    encryption::EncryptionService* encryption_service,
    std::unique_ptr<PageDb> page_db, PageId page_id)
    : environment_(environment),
      encryption_service_(encryption_service),
      page_id_(std::move(page_id)),
      db_(std::move(page_db)),
      page_sync_(nullptr),
      coroutine_manager_(environment->coroutine_service()) {}

PageStorageImpl::~PageStorageImpl() {}

void PageStorageImpl::Init(fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(SynchronousInit(handler));
      });
}

PageId PageStorageImpl::GetId() { return page_id_; }

void PageStorageImpl::SetSyncDelegate(PageSyncDelegate* page_sync) {
  page_sync_ = page_sync;
}

void PageStorageImpl::GetHeadCommitIds(
    fit::function<void(Status, std::vector<CommitId>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler,
             fit::function<void(Status, std::vector<CommitId>)> callback) {
        std::vector<CommitId> commit_ids;
        Status status = db_->GetHeads(handler, &commit_ids);
        callback(status, std::move(commit_ids));
      });
}

void PageStorageImpl::GetCommit(
    CommitIdView commit_id,
    fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  FXL_DCHECK(commit_id.size());
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit_id = commit_id.ToString()](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
        std::unique_ptr<const Commit> commit;
        Status status = SynchronousGetCommit(handler, commit_id, &commit);
        callback(status, std::move(commit));
      });
}

void PageStorageImpl::AddCommitFromLocal(
    std::unique_ptr<const Commit> commit,
    std::vector<ObjectIdentifier> new_objects,
    fit::function<void(Status)> callback) {
  FXL_DCHECK(IsDigestValid(commit->GetRootIdentifier().object_digest()));
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit = std::move(commit), new_objects = std::move(new_objects)](
          CoroutineHandler* handler,
          fit::function<void(Status)> callback) mutable {
        callback(SynchronousAddCommitFromLocal(handler, std::move(commit),
                                               std::move(new_objects)));
      });
}

void PageStorageImpl::AddCommitsFromSync(
    std::vector<CommitIdAndBytes> ids_and_bytes, ChangeSource source,
    fit::function<void(Status, std::vector<CommitId>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, ids_and_bytes = std::move(ids_and_bytes), source](
          CoroutineHandler* handler,
          fit::function<void(Status, std::vector<CommitId>)> callback) mutable {
        std::vector<CommitId> missing_ids;
        Status status = SynchronousAddCommitsFromSync(
            handler, std::move(ids_and_bytes), source, &missing_ids);
        callback(status, std::move(missing_ids));
      });
}

void PageStorageImpl::StartCommit(
    const CommitId& commit_id, JournalType journal_type,
    fit::function<void(Status, std::unique_ptr<Journal>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit_id, journal_type](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<Journal>)> callback) {
        JournalId journal_id;
        Status status =
            db_->CreateJournalId(handler, journal_type, commit_id, &journal_id);
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        std::unique_ptr<Journal> journal = JournalImpl::Simple(
            journal_type, environment_, this, journal_id, commit_id);
        callback(Status::OK, std::move(journal));
      });
}

void PageStorageImpl::StartMergeCommit(
    const CommitId& left, const CommitId& right,
    fit::function<void(Status, std::unique_ptr<Journal>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, left, right](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<Journal>)> callback) {
        JournalId journal_id;
        Status status = db_->CreateJournalId(handler, JournalType::EXPLICIT,
                                             left, &journal_id);
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        std::unique_ptr<Journal> journal =
            JournalImpl::Merge(environment_, this, journal_id, left, right);
        callback(Status::OK, std::move(journal));
      });
}

void PageStorageImpl::CommitJournal(
    std::unique_ptr<Journal> journal,
    fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  FXL_DCHECK(journal);

  auto managed_journal = managed_container_.Manage(std::move(journal));
  JournalImpl* journal_ptr = static_cast<JournalImpl*>(managed_journal->get());

  journal_ptr->Commit(
      [journal_ptr, managed_journal = std::move(managed_journal),
       callback = std::move(callback)](
          Status status, std::unique_ptr<const Commit> commit) mutable {
        if (status != Status::OK) {
          // Commit failed, roll the journal back.
          journal_ptr->Rollback(
              [status, managed_journal = std::move(managed_journal),
               callback = std::move(callback)](Status /*rollback_status*/) {
                callback(status, nullptr);
              });
          return;
        }
        callback(Status::OK, std::move(commit));
      });
}

void PageStorageImpl::RollbackJournal(std::unique_ptr<Journal> journal,
                                      fit::function<void(Status)> callback) {
  FXL_DCHECK(journal);

  auto managed_journal = managed_container_.Manage(std::move(journal));
  JournalImpl* journal_ptr = static_cast<JournalImpl*>(managed_journal->get());

  journal_ptr->Rollback(
      [managed_journal = std::move(managed_journal),
       callback = std::move(callback)](Status status) { callback(status); });
}

Status PageStorageImpl::AddCommitWatcher(CommitWatcher* watcher) {
  watchers_.push_back(watcher);
  return Status::OK;
}

Status PageStorageImpl::RemoveCommitWatcher(CommitWatcher* watcher) {
  auto watcher_it =
      std::find_if(watchers_.begin(), watchers_.end(),
                   [watcher](CommitWatcher* w) { return w == watcher; });
  if (watcher_it == watchers_.end()) {
    return Status::NOT_FOUND;
  }
  watchers_.erase(watcher_it);
  return Status::OK;
}

void PageStorageImpl::IsSynced(fit::function<void(Status, bool)> callback) {
  auto waiter = fxl::MakeRefCounted<callback::Waiter<Status, bool>>(Status::OK);
  // Check for unsynced commits.
  coroutine_manager_.StartCoroutine(
      waiter->NewCallback(),
      [this](CoroutineHandler* handler,
             fit::function<void(Status, bool)> callback) {
        std::vector<CommitId> commit_ids;
        Status status = db_->GetUnsyncedCommitIds(handler, &commit_ids);
        if (status != Status::OK) {
          callback(status, false);
        } else {
          callback(Status::OK, commit_ids.empty());
        }
      });

  // Check for unsynced pieces.
  GetUnsyncedPieces([pieces_callback = waiter->NewCallback()](
                        Status status, std::vector<ObjectIdentifier> pieces) {
    if (status != Status::OK) {
      pieces_callback(status, false);
    } else {
      pieces_callback(Status::OK, pieces.empty());
    }
  });

  waiter->Finalize([callback = std::move(callback)](
                       Status status, std::vector<bool> is_synced) {
    if (status != Status::OK) {
      callback(status, false);
      return;
    }
    FXL_DCHECK(is_synced.size() == 2);
    callback(Status::OK, is_synced[0] && is_synced[1]);
  });
}

bool PageStorageImpl::IsOnline() { return page_is_online_; }

void PageStorageImpl::IsEmpty(fit::function<void(Status, bool)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this](CoroutineHandler* handler,
                                  fit::function<void(Status, bool)> callback) {
        // Check there is a single head.
        std::vector<CommitId> commit_ids;
        Status status = db_->GetHeads(handler, &commit_ids);
        if (status != Status::OK) {
          callback(status, false);
          return;
        }
        FXL_DCHECK(!commit_ids.empty());
        if (commit_ids.size() > 1) {
          // A page is not empty if there is more than one head commit.
          callback(Status::OK, false);
          return;
        }
        // Compare the root node of the head commit to that of the empty node.
        std::unique_ptr<const Commit> commit;
        status = SynchronousGetCommit(handler, commit_ids[0], &commit);
        ObjectIdentifier* empty_node_id;
        status = SynchronousGetEmptyNodeIdentifier(handler, &empty_node_id);
        if (status != Status::OK) {
          callback(status, false);
          return;
        }
        callback(Status::OK, commit->GetRootIdentifier() == *empty_node_id);
      });
}

void PageStorageImpl::GetUnsyncedCommits(
    fit::function<void(Status, std::vector<std::unique_ptr<const Commit>>)>
        callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler,
             fit::function<void(Status,
                                std::vector<std::unique_ptr<const Commit>>)>
                 callback) {
        std::vector<std::unique_ptr<const Commit>> unsynced_commits;
        Status s = SynchronousGetUnsyncedCommits(handler, &unsynced_commits);
        callback(s, std::move(unsynced_commits));
      });
}

void PageStorageImpl::MarkCommitSynced(const CommitId& commit_id,
                                       fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit_id](CoroutineHandler* handler,
                        fit::function<void(Status)> callback) {
        callback(SynchronousMarkCommitSynced(handler, commit_id));
      });
}

void PageStorageImpl::GetUnsyncedPieces(
    fit::function<void(Status, std::vector<ObjectIdentifier>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](
          CoroutineHandler* handler,
          fit::function<void(Status, std::vector<ObjectIdentifier>)> callback) {
        std::vector<ObjectIdentifier> unsynced_object_identifiers;
        Status s =
            db_->GetUnsyncedPieces(handler, &unsynced_object_identifiers);
        callback(s, unsynced_object_identifiers);
      });
}

void PageStorageImpl::MarkPieceSynced(ObjectIdentifier object_identifier,
                                      fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->SetObjectStatus(handler, object_identifier,
                                      PageDbObjectStatus::SYNCED));
      });
}

void PageStorageImpl::IsPieceSynced(
    ObjectIdentifier object_identifier,
    fit::function<void(Status, bool)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler,
          fit::function<void(Status, bool)> callback) {
        PageDbObjectStatus object_status;
        Status status =
            db_->GetObjectStatus(handler, object_identifier, &object_status);
        callback(status, object_status == PageDbObjectStatus::SYNCED);
      });
}

void PageStorageImpl::MarkSyncedToPeer(fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      [this, callback = std::move(callback)](CoroutineHandler* handler) {
        std::unique_ptr<PageDb::Batch> batch;
        Status status = db_->StartBatch(handler, &batch);
        if (status != Status::OK) {
          callback(status);
          return;
        }
        status = SynchronousMarkPageOnline(handler, batch.get());
        if (status != Status::OK) {
          callback(status);
          return;
        }
        callback(batch->Execute(handler));
      });
}

void PageStorageImpl::AddObjectFromLocal(
    ObjectType object_type, std::unique_ptr<DataSource> data_source,
    fit::function<void(Status, ObjectIdentifier)> callback) {
  auto traced_callback =
      TRACE_CALLBACK(std::move(callback), "ledger", "page_storage_add_object");

  auto managed_data_source = managed_container_.Manage(std::move(data_source));
  auto managed_data_source_ptr = managed_data_source->get();
  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  SplitDataSource(
      managed_data_source_ptr, object_type,
      [this](ObjectDigest object_digest) {
        FXL_DCHECK(IsDigestValid(object_digest));
        return encryption_service_->MakeObjectIdentifier(
            std::move(object_digest));
      },
      [this, waiter, managed_data_source = std::move(managed_data_source),
       callback = std::move(traced_callback)](
          IterationStatus status, ObjectIdentifier identifier,
          std::unique_ptr<DataSource::DataChunk> chunk) mutable {
        if (status == IterationStatus::ERROR) {
          callback(Status::IO_ERROR, ObjectIdentifier());
          return;
        }

        FXL_DCHECK(chunk != nullptr);

        if (!GetObjectDigestInfo(identifier.object_digest()).is_inlined()) {
          AddPiece(identifier, ChangeSource::LOCAL, IsObjectSynced::NO,
                   std::move(chunk), waiter->NewCallback());
        }
        if (status == IterationStatus::IN_PROGRESS)
          return;

        FXL_DCHECK(status == IterationStatus::DONE);
        waiter->Finalize(
            [identifier = std::move(identifier),
             callback = std::move(callback)](Status status) mutable {
              callback(status, std::move(identifier));
            });
      });
}

void PageStorageImpl::GetObjectPart(
    ObjectIdentifier object_identifier, int64_t offset, int64_t max_size,
    Location location, fit::function<void(Status, fsl::SizedVmo)> callback) {
  FXL_DCHECK(IsDigestValid(object_identifier.object_digest()));
  GetPiece(
      object_identifier,
      [this, object_identifier, offset, max_size, location,
       callback = std::move(callback)](
          Status status, std::unique_ptr<const Object> object) mutable {
        if (status == Status::NOT_FOUND) {
          if (location == Location::NETWORK) {
            GetObjectPartFromSync(object_identifier, offset, max_size,
                                  std::move(callback));
          } else {
            callback(Status::NOT_FOUND, nullptr);
          }
          return;
        }

        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        FXL_DCHECK(object);
        ObjectDigestInfo digest_info =
            GetObjectDigestInfo(object_identifier.object_digest());
        fxl::StringView data;
        Status get_data_status = object->GetData(&data);
        if (get_data_status != Status::OK) {
          callback(get_data_status, nullptr);
          return;
        }

        if (digest_info.is_inlined() || digest_info.is_chunk()) {
          fsl::SizedVmo buffer;
          size_t start = GetObjectPartStart(offset, data.size());
          size_t length = GetObjectPartLength(max_size, data.size(), start);

          if (!fsl::VmoFromString(data.substr(start, length), &buffer)) {
            callback(Status::INTERNAL_IO_ERROR, nullptr);
            return;
          }
          callback(Status::OK, std::move(buffer));
          return;
        }

        FXL_DCHECK(digest_info.piece_type == PieceType::INDEX);
        const FileIndex* file_index;
        status = FileIndexSerialization::ParseFileIndex(data, &file_index);
        if (status != Status::OK) {
          callback(Status::FORMAT_ERROR, nullptr);
          return;
        }
        size_t start = GetObjectPartStart(offset, file_index->size());
        size_t length =
            GetObjectPartLength(max_size, file_index->size(), start);

        zx::vmo raw_vmo;
        zx_status_t zx_status = zx::vmo::create(length, 0, &raw_vmo);
        if (zx_status != ZX_OK) {
          FXL_LOG(WARNING) << "Unable to create VMO of size: " << length;
          callback(Status::INTERNAL_IO_ERROR, nullptr);
          return;
        }
        fsl::SizedVmo vmo(std::move(raw_vmo), length);

        if (length == 0) {
          callback(Status::OK, std::move(vmo));
        }

        fsl::SizedVmo vmo_copy;
        zx_status = vmo.Duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_WRITE, &vmo_copy);
        if (zx_status != ZX_OK) {
          FXL_LOG(ERROR) << "Unable to duplicate vmo. Status: " << zx_status;
          callback(Status::INTERNAL_IO_ERROR, nullptr);
          return;
        }
        FillBufferWithObjectContent(
            std::move(object), std::move(vmo_copy), start, length, 0,
            file_index->size(),
            [vmo = std::move(vmo), callback = std::move(callback)](
                Status status) mutable { callback(status, std::move(vmo)); });
      });
}

void PageStorageImpl::GetObject(
    ObjectIdentifier object_identifier, Location location,
    fit::function<void(Status, std::unique_ptr<const Object>)> callback) {
  GetObjectPart(object_identifier, 0, -1, location,
                [object_identifier = std::move(object_identifier),
                 callback = std::move(callback)](Status status,
                                                 fsl::SizedVmo vmo) mutable {
                  callback(status,
                           std::make_unique<VmoObject>(
                               std::move(object_identifier), std::move(vmo)));
                });
}

void PageStorageImpl::GetPiece(
    ObjectIdentifier object_identifier,
    fit::function<void(Status, std::unique_ptr<const Object>)> callback) {
  ObjectDigestInfo digest_info =
      GetObjectDigestInfo(object_identifier.object_digest());
  if (digest_info.is_inlined()) {
    callback(Status::OK,
             std::make_unique<InlinedObject>(std::move(object_identifier)));
    return;
  }

  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<const Object>)>
              callback) mutable {
        std::unique_ptr<const Object> object;
        Status status =
            db_->ReadObject(handler, std::move(object_identifier), &object);
        callback(status, std::move(object));
      });
}

void PageStorageImpl::SetSyncMetadata(fxl::StringView key,
                                      fxl::StringView value,
                                      fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, key = key.ToString(), value = value.ToString()](
          CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->SetSyncMetadata(handler, key, value));
      });
}

void PageStorageImpl::GetSyncMetadata(
    fxl::StringView key, fit::function<void(Status, std::string)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, key = key.ToString()](
          CoroutineHandler* handler,
          fit::function<void(Status, std::string)> callback) {
        std::string value;
        Status status = db_->GetSyncMetadata(handler, key, &value);
        callback(status, std::move(value));
      });
}

void PageStorageImpl::GetCommitContents(const Commit& commit,
                                        std::string min_key,
                                        fit::function<bool(Entry)> on_next,
                                        fit::function<void(Status)> on_done) {
  btree::ForEachEntry(
      environment_->coroutine_service(), this, commit.GetRootIdentifier(),
      min_key,
      [on_next = std::move(on_next)](btree::EntryAndNodeIdentifier next) {
        return on_next(next.entry);
      },
      std::move(on_done));
}

void PageStorageImpl::GetEntryFromCommit(
    const Commit& commit, std::string key,
    fit::function<void(Status, Entry)> callback) {
  std::unique_ptr<bool> key_found = std::make_unique<bool>(false);
  auto on_next = [key, key_found = key_found.get(),
                  callback =
                      callback.share()](btree::EntryAndNodeIdentifier next) {
    if (next.entry.key == key) {
      *key_found = true;
      callback(Status::OK, next.entry);
    }
    return false;
  };

  auto on_done = [key_found = std::move(key_found),
                  callback = std::move(callback)](Status s) {
    if (*key_found) {
      return;
    }
    if (s == Status::OK) {
      callback(Status::NOT_FOUND, Entry());
      return;
    }
    callback(s, Entry());
  };
  btree::ForEachEntry(environment_->coroutine_service(), this,
                      commit.GetRootIdentifier(), std::move(key),
                      std::move(on_next), std::move(on_done));
}

void PageStorageImpl::GetCommitContentsDiff(
    const Commit& base_commit, const Commit& other_commit, std::string min_key,
    fit::function<bool(EntryChange)> on_next_diff,
    fit::function<void(Status)> on_done) {
  btree::ForEachDiff(environment_->coroutine_service(), this,
                     base_commit.GetRootIdentifier(),
                     other_commit.GetRootIdentifier(), std::move(min_key),
                     std::move(on_next_diff), std::move(on_done));
}

void PageStorageImpl::GetThreeWayContentsDiff(
    const Commit& base_commit, const Commit& left_commit,
    const Commit& right_commit, std::string min_key,
    fit::function<bool(ThreeWayChange)> on_next_diff,
    fit::function<void(Status)> on_done) {
  btree::ForEachThreeWayDiff(
      environment_->coroutine_service(), this, base_commit.GetRootIdentifier(),
      left_commit.GetRootIdentifier(), right_commit.GetRootIdentifier(),
      std::move(min_key), std::move(on_next_diff), std::move(on_done));
}

void PageStorageImpl::GetJournalEntries(
    const JournalId& journal_id,
    fit::function<void(Status, std::unique_ptr<Iterator<const EntryChange>>,
                       JournalContainsClearOperation contains_clear_operation)>
        callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal_id](
          CoroutineHandler* handler,
          fit::function<void(
              Status, std::unique_ptr<Iterator<const EntryChange>>,
              JournalContainsClearOperation contains_clear_operation)>
              callback) {
        std::unique_ptr<Iterator<const EntryChange>> entries;
        JournalContainsClearOperation contains_clear_operation;
        Status s = db_->GetJournalEntries(handler, journal_id, &entries,
                                          &contains_clear_operation);
        callback(s, std::move(entries), contains_clear_operation);
      });
}

void PageStorageImpl::AddJournalEntry(const JournalId& journal_id,
                                      fxl::StringView key,
                                      ObjectIdentifier object_identifier,
                                      KeyPriority priority,
                                      fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal_id, key = key.ToString(),
       object_identifier = std::move(object_identifier), priority](
          CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->AddJournalEntry(handler, journal_id, key,
                                      object_identifier, priority));
      });
}

void PageStorageImpl::RemoveJournalEntry(const JournalId& journal_id,
                                         convert::ExtendedStringView key,
                                         fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal_id, key = key.ToString()](
          CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->RemoveJournalEntry(handler, journal_id, key));
      });
}

void PageStorageImpl::EmptyJournalAndMarkContainsClearOperation(
    const JournalId& journal_id, fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal_id](CoroutineHandler* handler,
                         fit::function<void(Status)> callback) {
        callback(db_->EmptyJournalAndMarkContainsClearOperation(handler,
                                                                journal_id));
      });
}

void PageStorageImpl::RemoveJournal(const JournalId& journal_id,
                                    fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal_id](CoroutineHandler* handler,
                         fit::function<void(Status)> callback) {
        callback(db_->RemoveJournal(handler, journal_id));
      });
}

void PageStorageImpl::NotifyWatchersOfNewCommits(
    const std::vector<std::unique_ptr<const Commit>>& new_commits,
    ChangeSource source) {
  for (CommitWatcher* watcher : watchers_) {
    watcher->OnNewCommits(new_commits, source);
  }
}

Status PageStorageImpl::MarkAllPiecesLocal(
    CoroutineHandler* handler, PageDb::Batch* batch,
    std::vector<ObjectIdentifier> object_identifiers) {
  std::set<ObjectIdentifier> seen_identifiers;
  while (!object_identifiers.empty()) {
    auto it = seen_identifiers.insert(std::move(object_identifiers.back()));
    object_identifiers.pop_back();
    const ObjectIdentifier& object_identifier = *(it.first);
    FXL_DCHECK(
        !GetObjectDigestInfo(object_identifier.object_digest()).is_inlined());
    Status status = batch->SetObjectStatus(handler, object_identifier,
                                           PageDbObjectStatus::LOCAL);
    if (status != Status::OK) {
      return status;
    }
    if (GetObjectDigestInfo(object_identifier.object_digest()).piece_type ==
        PieceType::INDEX) {
      std::unique_ptr<const Object> object;
      status = db_->ReadObject(handler, object_identifier, &object);
      if (status != Status::OK) {
        return status;
      }

      fxl::StringView content;
      status = object->GetData(&content);
      if (status != Status::OK) {
        return status;
      }

      const FileIndex* file_index;
      status = FileIndexSerialization::ParseFileIndex(content, &file_index);
      if (status != Status::OK) {
        return status;
      }

      object_identifiers.reserve(object_identifiers.size() +
                                 file_index->children()->size());
      for (const auto* child : *file_index->children()) {
        ObjectIdentifier new_object_identifier =
            ToObjectIdentifier(child->object_identifier());
        if (!GetObjectDigestInfo(new_object_identifier.object_digest())
                 .is_inlined()) {
          if (!seen_identifiers.count(new_object_identifier)) {
            object_identifiers.push_back(std::move(new_object_identifier));
          }
        }
      }
    }
  }
  return Status::OK;
}

Status PageStorageImpl::ContainsCommit(CoroutineHandler* handler,
                                       CommitIdView id) {
  if (IsFirstCommit(id)) {
    return Status::OK;
  }
  std::string bytes;
  return db_->GetCommitStorageBytes(handler, id, &bytes);
}

bool PageStorageImpl::IsFirstCommit(CommitIdView id) {
  return id == kFirstPageCommitId;
}

void PageStorageImpl::AddPiece(ObjectIdentifier object_identifier,
                               ChangeSource source,
                               IsObjectSynced is_object_synced,
                               std::unique_ptr<DataSource::DataChunk> data,
                               fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier),
       data = std::move(data), source,
       is_object_synced](CoroutineHandler* handler,
                         fit::function<void(Status)> callback) mutable {
        callback(SynchronousAddPiece(handler, std::move(object_identifier),
                                     source, is_object_synced,
                                     std::move(data)));
      });
}

void PageStorageImpl::DownloadFullObject(ObjectIdentifier object_identifier,
                                         fit::function<void(Status)> callback) {
  FXL_DCHECK(page_sync_);
  FXL_DCHECK(
      !GetObjectDigestInfo(object_identifier.object_digest()).is_inlined());

  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler,
          fit::function<void(Status)> callback) mutable {
        Status status;
        ChangeSource source;
        IsObjectSynced is_object_synced;
        std::unique_ptr<DataSource::DataChunk> chunk;

        if (coroutine::SyncCall(
                handler,
                [this, object_identifier](
                    fit::function<void(Status, ChangeSource, IsObjectSynced,
                                       std::unique_ptr<DataSource::DataChunk>)>
                        callback) mutable {
                  page_sync_->GetObject(std::move(object_identifier),
                                        std::move(callback));
                },
                &status, &source, &is_object_synced,
                &chunk) == coroutine::ContinuationStatus::INTERRUPTED) {
          callback(Status::INTERRUPTED);
          return;
        }

        if (status != Status::OK) {
          callback(status);
          return;
        }
        auto digest_info =
            GetObjectDigestInfo(object_identifier.object_digest());
        FXL_DCHECK(!digest_info.is_inlined());

        if (object_identifier.object_digest() !=
            ComputeObjectDigest(digest_info.piece_type, digest_info.object_type,
                                chunk->Get())) {
          callback(Status::OBJECT_DIGEST_MISMATCH);
          return;
        }

        if (digest_info.is_chunk()) {
          callback(SynchronousAddPiece(handler, std::move(object_identifier),
                                       source, is_object_synced,
                                       std::move(chunk)));
          return;
        }

        auto waiter =
            fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
        status = ForEachPiece(chunk->Get(), [&](ObjectIdentifier identifier) {
          if (GetObjectDigestInfo(identifier.object_digest()).is_inlined()) {
            return Status::OK;
          }

          Status status = db_->ReadObject(handler, identifier, nullptr);
          if (status == Status::NOT_FOUND) {
            DownloadFullObject(std::move(identifier), waiter->NewCallback());
            return Status::OK;
          }
          return status;
        });
        if (status != Status::OK) {
          callback(status);
          return;
        }

        if (coroutine::Wait(handler, std::move(waiter), &status) ==
            coroutine::ContinuationStatus::INTERRUPTED) {
          callback(Status::INTERRUPTED);
          return;
        }

        if (status != Status::OK) {
          callback(status);
          return;
        }

        callback(SynchronousAddPiece(handler, std::move(object_identifier),
                                     source, is_object_synced,
                                     std::move(chunk)));
      });
}

void PageStorageImpl::GetObjectPartFromSync(
    ObjectIdentifier object_identifier, size_t offset, size_t size,
    fit::function<void(Status, fsl::SizedVmo)> callback) {
  if (!page_sync_) {
    callback(Status::NOT_CONNECTED_ERROR, nullptr);
    return;
  }

  // TODO(LE-239): download only the needed part.
  DownloadFullObject(object_identifier,
                     [this, object_identifier, offset, size,
                      callback = std::move(callback)](Status status) mutable {
                       if (status != Status::OK) {
                         callback(status, nullptr);
                         return;
                       }

                       GetObjectPart(object_identifier, offset, size,
                                     Location::LOCAL, std::move(callback));
                     });
}

void PageStorageImpl::ObjectIsUntracked(
    ObjectIdentifier object_identifier,
    fit::function<void(Status, bool)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler,
          fit::function<void(Status, bool)> callback) mutable {
        if (GetObjectDigestInfo(object_identifier.object_digest())
                .is_inlined()) {
          callback(Status::OK, false);
          return;
        }

        PageDbObjectStatus object_status;
        Status status =
            db_->GetObjectStatus(handler, object_identifier, &object_status);
        callback(status, object_status == PageDbObjectStatus::TRANSIENT);
      });
}

void PageStorageImpl::FillBufferWithObjectContent(
    ObjectIdentifier object_identifier, fsl::SizedVmo vmo,
    int64_t global_offset, size_t global_size, int64_t current_position,
    size_t object_size, fit::function<void(Status)> callback) {
  GetPiece(object_identifier,
           [this, vmo = std::move(vmo), global_offset, global_size,
            current_position, object_size, callback = std::move(callback)](
               Status status, std::unique_ptr<const Object> object) mutable {
             if (status != Status::OK) {
               callback(status);
               return;
             }

             FXL_DCHECK(object);
             FillBufferWithObjectContent(
                 std::move(object), std::move(vmo), global_offset, global_size,
                 current_position, object_size, std::move(callback));
           });
}

void PageStorageImpl::FillBufferWithObjectContent(
    std::unique_ptr<const Object> object, fsl::SizedVmo vmo,
    int64_t global_offset, size_t global_size, int64_t current_position,
    size_t object_size, fit::function<void(Status)> callback) {
  fxl::StringView content;
  Status status = object->GetData(&content);
  if (status != Status::OK) {
    callback(status);
    return;
  }
  ObjectDigestInfo digest_info =
      GetObjectDigestInfo(object->GetIdentifier().object_digest());
  if (digest_info.is_inlined() || digest_info.is_chunk()) {
    if (object_size != content.size()) {
      FXL_LOG(ERROR) << "Error in serialization format. Expecting object: "
                     << object->GetIdentifier()
                     << " to have size: " << object_size
                     << ", but found an object of size: " << content.size();
      callback(Status::FORMAT_ERROR);
      return;
    }
    // Distance is negative if the offset is ahead and positive if behind.
    int64_t distance_from_global_offset = current_position - global_offset;
    // Read offset can be non-zero on first read; in that case, we need to skip
    // bytes coming before global offset.
    size_t read_offset = std::max(-distance_from_global_offset, 0L);
    // Write offset is zero on the first write; otherwise we need to skip number
    // of bytes corresponding to what we have already written.
    size_t write_offset = std::max(distance_from_global_offset, 0L);
    // Read and write until reaching either size of the object, or global size.
    size_t read_write_size =
        std::min(content.size() - read_offset, global_size - write_offset);
    FXL_DCHECK(read_write_size > 0);
    fxl::StringView read_substr = content.substr(read_offset, read_write_size);
    zx_status_t zx_status =
        vmo.vmo().write(read_substr.data(), write_offset, read_write_size);
    if (zx_status != ZX_OK) {
      FXL_LOG(ERROR) << "Unable to write to vmo. Status: " << zx_status;
      callback(Status::INTERNAL_IO_ERROR);
      return;
    }
    callback(Status::OK);
    return;
  }

  const FileIndex* file_index;
  status = FileIndexSerialization::ParseFileIndex(content, &file_index);
  if (status != Status::OK) {
    callback(Status::FORMAT_ERROR);
    return;
  }
  if (file_index->size() != object_size) {
    FXL_LOG(ERROR) << "Error in serialization format. Expecting object: "
                   << object->GetIdentifier() << " to have size " << object_size
                   << ", but found an index object of size: "
                   << file_index->size();
    callback(Status::FORMAT_ERROR);
    return;
  }

  int64_t sub_offset = 0;
  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  for (const auto* child : *file_index->children()) {
    if (sub_offset + child->size() > file_index->size()) {
      callback(Status::FORMAT_ERROR);
      return;
    }
    int64_t child_position = current_position + sub_offset;
    ObjectIdentifier child_identifier =
        ToObjectIdentifier(child->object_identifier());
    if (child_position + static_cast<int64_t>(child->size()) < global_offset) {
      sub_offset += child->size();
      continue;
    }
    if (global_offset + static_cast<int64_t>(global_size) < child_position) {
      break;
    }
    fsl::SizedVmo vmo_copy;
    zx_status_t zx_status =
        vmo.Duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_WRITE, &vmo_copy);
    if (zx_status != ZX_OK) {
      FXL_LOG(ERROR) << "Unable to duplicate vmo. Status: " << zx_status;
      callback(Status::INTERNAL_IO_ERROR);
      return;
    }
    FillBufferWithObjectContent(child_identifier, std::move(vmo_copy),
                                global_offset, global_size, child_position,
                                child->size(), waiter->NewCallback());
    sub_offset += child->size();
  }
  waiter->Finalize(std::move(callback));
}

Status PageStorageImpl::SynchronousInit(CoroutineHandler* handler) {
  // Add the default page head if this page is empty.
  std::vector<CommitId> heads;
  Status s = db_->GetHeads(handler, &heads);
  if (s != Status::OK) {
    return s;
  }
  if (heads.empty()) {
    s = db_->AddHead(handler, kFirstPageCommitId, zx::time_utc());
    if (s != Status::OK) {
      return s;
    }
  }

  // Cache whether this page is online or not.
  s = db_->IsPageOnline(handler, &page_is_online_);
  if (s != Status::OK) {
    return s;
  }

  // Remove uncommited explicit journals.
  if (db_->RemoveExplicitJournals(handler) == Status::INTERRUPTED) {
    // Only fail if the handler is invalidated. Otherwise, failure to remove
    // explicit journals should not block the initalization.
    return Status::INTERRUPTED;
  }

  // Commit uncommited implicit journals.
  std::vector<JournalId> journal_ids;
  s = db_->GetImplicitJournalIds(handler, &journal_ids);
  if (s != Status::OK) {
    return s;
  }

  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  for (JournalId& id : journal_ids) {
    CommitId base;
    s = db_->GetBaseCommitForJournal(handler, id, &base);
    if (s != Status::OK) {
      FXL_LOG(ERROR) << "Failed to get implicit journal with status " << s
                     << ". journal id: " << id;
      return s;
    }
    std::unique_ptr<Journal> journal = JournalImpl::Simple(
        JournalType::IMPLICIT, environment_, this, id, base);

    CommitJournal(
        std::move(journal), [status_callback = waiter->NewCallback()](
                                Status status, std::unique_ptr<const Commit>) {
          if (status != Status::OK) {
            FXL_LOG(ERROR) << "Failed to commit implicit journal created in "
                              "previous Ledger execution.";
          }
          status_callback(status);
        });
  }

  if (coroutine::Wait(handler, std::move(waiter), &s) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return s;
}

Status PageStorageImpl::SynchronousGetCommit(
    CoroutineHandler* handler, CommitId commit_id,
    std::unique_ptr<const Commit>* commit) {
  if (IsFirstCommit(commit_id)) {
    Status s;
    if (coroutine::SyncCall(
            handler,
            [this](fit::function<void(Status, std::unique_ptr<const Commit>)>
                       callback) {
              CommitImpl::Empty(this, std::move(callback));
            },
            &s, commit) == coroutine::ContinuationStatus::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    return s;
  }
  std::string bytes;
  Status s = db_->GetCommitStorageBytes(handler, commit_id, &bytes);
  if (s != Status::OK) {
    return s;
  }
  return CommitImpl::FromStorageBytes(this, commit_id, std::move(bytes),
                                      commit);
}

Status PageStorageImpl::SynchronousAddCommitFromLocal(
    CoroutineHandler* handler, std::unique_ptr<const Commit> commit,
    std::vector<ObjectIdentifier> new_objects) {
  std::vector<std::unique_ptr<const Commit>> commits;
  commits.reserve(1);
  commits.push_back(std::move(commit));

  return SynchronousAddCommits(handler, std::move(commits), ChangeSource::LOCAL,
                               std::move(new_objects), nullptr);
}

Status PageStorageImpl::SynchronousAddCommitsFromSync(
    CoroutineHandler* handler, std::vector<CommitIdAndBytes> ids_and_bytes,
    ChangeSource source, std::vector<CommitId>* missing_ids) {
  std::vector<std::unique_ptr<const Commit>> commits;

  std::map<const CommitId*, const Commit*, StringPointerComparator> leaves;
  commits.reserve(ids_and_bytes.size());

  // The locked section below contains asynchronous operations reading the
  // database, and branches depending on those reads. This section is thus a
  // critical section and we need to ensure it is not executed concurrently by
  // several coroutines. The locked sections (and only those) are thus executed
  // serially.
  std::unique_ptr<lock::Lock> lock;
  if (lock::AcquireLock(handler, &commit_serializer_, &lock) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }

  for (auto& id_and_bytes : ids_and_bytes) {
    CommitId id = std::move(id_and_bytes.id);
    std::string storage_bytes = std::move(id_and_bytes.bytes);
    Status status = ContainsCommit(handler, id);
    if (status == Status::OK) {
      // We only mark cloud-sourced commits as synced.
      if (source == ChangeSource::CLOUD) {
        Status status = SynchronousMarkCommitSynced(handler, id);
        if (status != Status::OK) {
          return status;
        }
      }
      continue;
    }

    if (status != Status::NOT_FOUND) {
      return status;
    }

    std::unique_ptr<const Commit> commit;
    status = CommitImpl::FromStorageBytes(this, id, std::move(storage_bytes),
                                          &commit);
    if (status != Status::OK) {
      FXL_LOG(ERROR) << "Unable to add commit. Id: " << convert::ToHex(id);
      return status;
    }

    // Remove parents from leaves.
    for (const auto& parent_id : commit->GetParentIds()) {
      auto it = leaves.find(&parent_id);
      if (it != leaves.end()) {
        leaves.erase(it);
      }
    }
    leaves[&commit->GetId()] = commit.get();
    commits.push_back(std::move(commit));
  }

  if (commits.empty()) {
    return Status::OK;
  }

  lock.reset();

  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  // Get all objects from sync and then add the commit objects.
  for (const auto& leaf : leaves) {
    btree::GetObjectsFromSync(environment_->coroutine_service(), this,
                              leaf.second->GetRootIdentifier(),
                              waiter->NewCallback());
  }

  Status waiter_status;
  if (coroutine::Wait(handler, std::move(waiter), &waiter_status) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  if (waiter_status != Status::OK) {
    return waiter_status;
  }

  return SynchronousAddCommits(handler, std::move(commits), source,
                               std::vector<ObjectIdentifier>(), missing_ids);
}

Status PageStorageImpl::SynchronousGetUnsyncedCommits(
    CoroutineHandler* handler,
    std::vector<std::unique_ptr<const Commit>>* unsynced_commits) {
  std::vector<CommitId> commit_ids;
  Status s = db_->GetUnsyncedCommitIds(handler, &commit_ids);
  if (s != Status::OK) {
    return s;
  }

  auto waiter = fxl::MakeRefCounted<
      callback::Waiter<Status, std::unique_ptr<const Commit>>>(Status::OK);
  for (const auto& commit_id : commit_ids) {
    GetCommit(commit_id, waiter->NewCallback());
  }

  std::vector<std::unique_ptr<const Commit>> result;
  if (coroutine::Wait(handler, std::move(waiter), &s, &result) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  if (s != Status::OK) {
    return s;
  }
  unsynced_commits->swap(result);
  return Status::OK;
}

Status PageStorageImpl::SynchronousMarkCommitSynced(CoroutineHandler* handler,
                                                    const CommitId& commit_id) {
  std::unique_ptr<PageDb::Batch> batch;
  Status status = db_->StartBatch(handler, &batch);
  if (status != Status::OK) {
    return status;
  }
  status = SynchronousMarkCommitSyncedInBatch(handler, batch.get(), commit_id);
  if (status != Status::OK) {
    return status;
  }
  return batch->Execute(handler);
}

Status PageStorageImpl::SynchronousMarkCommitSyncedInBatch(
    CoroutineHandler* handler, PageDb::Batch* batch,
    const CommitId& commit_id) {
  Status status = SynchronousMarkPageOnline(handler, batch);
  if (status != Status::OK) {
    return status;
  }
  return batch->MarkCommitIdSynced(handler, commit_id);
}

Status PageStorageImpl::SynchronousAddCommits(
    CoroutineHandler* handler,
    std::vector<std::unique_ptr<const Commit>> commits, ChangeSource source,
    std::vector<ObjectIdentifier> new_objects,
    std::vector<CommitId>* missing_ids) {
  // Make sure that only one AddCommits operation is executed at a time.
  // Otherwise, if db_ operations are asynchronous, ContainsCommit (below) may
  // return NOT_FOUND while another commit is added, and batch->Execute() will
  // break the invariants of this system (in particular, that synced commits
  // cannot become unsynced).
  std::unique_ptr<lock::Lock> lock;
  if (lock::AcquireLock(handler, &commit_serializer_, &lock) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }

  // Apply all changes atomically.
  std::unique_ptr<PageDb::Batch> batch;
  Status status = db_->StartBatch(handler, &batch);
  if (status != Status::OK) {
    return status;
  }
  std::set<const CommitId*, StringPointerComparator> added_commits;
  std::vector<std::unique_ptr<const Commit>> commits_to_send;

  std::map<CommitId, zx::time_utc> heads_to_add;

  int orphaned_commits = 0;
  for (auto& commit : commits) {
    // We need to check if we are adding an already present remote commit here
    // because we might both download and locally commit the same commit at
    // roughly the same time. As commit writing is asynchronous, the previous
    // check in AddCommitsFromSync may have not matched any commit, while a
    // commit got added in between.
    Status s = ContainsCommit(handler, commit->GetId());
    if (s == Status::OK) {
      if (source == ChangeSource::CLOUD) {
        s = SynchronousMarkCommitSyncedInBatch(handler, batch.get(),
                                               commit->GetId());
        if (s != Status::OK) {
          return s;
        }
      }
      // The commit is already here. We can safely skip it.
      continue;
    }
    if (s != Status::NOT_FOUND) {
      return s;
    }
    // Now, we know we are adding a new commit.

    // Commits should arrive in order. Check that the parents are either
    // present in PageDb or in the list of already processed commits.
    // If the commit arrive out of order, print an error, but skip it
    // temporarly so that the Ledger can recover if all the needed commits
    // are received in a single batch.
    bool orphaned_commit = false;
    for (const CommitIdView& parent_id : commit->GetParentIds()) {
      if (added_commits.find(&parent_id) == added_commits.end()) {
        s = ContainsCommit(handler, parent_id);
        if (s != Status::OK) {
          FXL_LOG(ERROR) << "Failed to find parent commit \""
                         << ToHex(parent_id) << "\" of commit \""
                         << convert::ToHex(commit->GetId()) << "\".";
          if (s == Status::NOT_FOUND) {
            if (missing_ids) {
              missing_ids->push_back(parent_id.ToString());
            }
            orphaned_commit = true;
            continue;
          }
          return Status::INTERNAL_IO_ERROR;
        }
      }
      // Remove the parent from the list of heads.
      if (!heads_to_add.erase(parent_id.ToString())) {
        // parent_id was not added in the batch: remove it from heads in Db.
        s = batch->RemoveHead(handler, parent_id);
        if (s != Status::OK) {
          return s;
        }
      }
    }

    // The commit could not be added. Skip it.
    if (orphaned_commit) {
      orphaned_commits++;
      continue;
    }

    s = batch->AddCommitStorageBytes(handler, commit->GetId(),
                                     commit->GetStorageBytes());
    if (s != Status::OK) {
      return s;
    }

    if (source != ChangeSource::CLOUD) {
      s = batch->MarkCommitIdUnsynced(handler, commit->GetId(),
                                      commit->GetGeneration());
      if (s != Status::OK) {
        return s;
      }
    }

    // Update heads_to_add.
    heads_to_add[commit->GetId()] = commit->GetTimestamp();

    added_commits.insert(&commit->GetId());
    commits_to_send.push_back(std::move(commit));
  }

  if (orphaned_commits > 0) {
    if (source != ChangeSource::P2P) {
      ledger::ReportEvent(
          ledger::CobaltEvent::COMMITS_RECEIVED_OUT_OF_ORDER_NOT_RECOVERED);
      FXL_LOG(ERROR)
          << "Failed adding commits. Found " << orphaned_commits
          << " orphaned commits (one of their parent was not found).";
    }
    return Status::NOT_FOUND;
  }

  // Update heads in Db.
  for (const auto& head_timestamp : heads_to_add) {
    Status s =
        batch->AddHead(handler, head_timestamp.first, head_timestamp.second);
    if (s != Status::OK) {
      return s;
    }
  }

  // If adding local commits, mark all new pieces as local.
  Status s = MarkAllPiecesLocal(handler, batch.get(), std::move(new_objects));
  if (s != Status::OK) {
    return s;
  }

  s = batch->Execute(handler);

  NotifyWatchersOfNewCommits(commits_to_send, source);

  return s;
}

Status PageStorageImpl::SynchronousAddPiece(
    CoroutineHandler* handler, ObjectIdentifier object_identifier,
    ChangeSource source, IsObjectSynced is_object_synced,
    std::unique_ptr<DataSource::DataChunk> data) {
  FXL_DCHECK(
      !GetObjectDigestInfo(object_identifier.object_digest()).is_inlined());
  FXL_DCHECK(
      object_identifier.object_digest() ==
      ComputeObjectDigest(
          GetObjectDigestInfo(object_identifier.object_digest()).piece_type,
          GetObjectDigestInfo(object_identifier.object_digest()).object_type,
          data->Get()));

  std::unique_ptr<const Object> object;
  Status status = db_->ReadObject(handler, object_identifier, &object);
  if (status == Status::NOT_FOUND) {
    PageDbObjectStatus object_status;
    switch (is_object_synced) {
      case IsObjectSynced::NO:
        object_status =
            (source == ChangeSource::LOCAL ? PageDbObjectStatus::TRANSIENT
                                           : PageDbObjectStatus::LOCAL);
        break;
      case IsObjectSynced::YES:
        object_status = PageDbObjectStatus::SYNCED;
        break;
    }
    return db_->WriteObject(handler, object_identifier, std::move(data),
                            object_status);
  }
  return status;
}

Status PageStorageImpl::SynchronousMarkPageOnline(
    coroutine::CoroutineHandler* handler, PageDb::Batch* batch) {
  if (page_is_online_) {
    return Status::OK;
  }
  Status status = batch->MarkPageOnline(handler);
  if (status == Status::OK) {
    page_is_online_ = true;
  }
  return status;
}

FXL_WARN_UNUSED_RESULT Status
PageStorageImpl::SynchronousGetEmptyNodeIdentifier(
    coroutine::CoroutineHandler* handler, ObjectIdentifier** empty_node_id) {
  if (!empty_node_id_) {
    // Get the empty node identifier and cache it.
    Status status;
    ObjectIdentifier object_identifier;
    if (coroutine::SyncCall(
            handler,
            [this](fit::function<void(Status, ObjectIdentifier)> callback) {
              btree::TreeNode::Empty(this, std::move(callback));
            },
            &status,
            &object_identifier) == coroutine::ContinuationStatus::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    if (status != Status::OK) {
      return status;
    }
    empty_node_id_ =
        std::make_unique<ObjectIdentifier>(std::move(object_identifier));
  }
  *empty_node_id = empty_node_id_.get();
  return Status::OK;
}

}  // namespace storage