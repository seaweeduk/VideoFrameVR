#include "stash_worker.h"

#include "media_client.h"

void StashWorkerShared::halt() {
  stop = true;
  if (client) client->cancel();
}
