/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsFaviconService_h_
#define nsFaviconService_h_

#include <utility>

#include "Database.h"
#include "FaviconHelpers.h"
#include "imgITools.h"
#include "mozilla/Attributes.h"
#include "mozilla/MozPromise.h"
#include "mozilla/storage.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsIFaviconService.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsTHashtable.h"
#include "nsToolkitCompsCID.h"
#include "nsURIHashKey.h"
#include "prtime.h"

// The target dimension in pixels for favicons we store, in reverse order.
// When adding/removing sizes from here, make sure to update the vector size.
extern const uint16_t gFaviconSizes[7];

// forward class definitions
class mozIStorageStatementCallback;

class nsFaviconService final : public nsIFaviconService {
 public:
  nsFaviconService();

  /**
   * Obtains the service's object.
   */
  static already_AddRefed<nsFaviconService> GetSingleton();

  /**
   * Initializes the service's object.  This should only be called once.
   */
  nsresult Init();

  /**
   * Returns a cached pointer to the favicon service for consumers in the
   * places directory.
   */
  static nsFaviconService* GetFaviconService() {
    if (!gFaviconService) {
      nsCOMPtr<nsIFaviconService> serv =
          do_GetService(NS_FAVICONSERVICE_CONTRACTID);
      NS_ENSURE_TRUE(serv, nullptr);
      NS_ASSERTION(gFaviconService, "Should have static instance pointer now");
    }
    return gFaviconService;
  }

  // addition to API for strings to prevent excessive parsing of URIs
  nsresult GetFaviconLinkForIconString(const nsCString& aSpec,
                                       nsIURI** aOutput);

  nsresult OptimizeIconSizes(mozilla::places::IconData& aIcon);

  /**
   * Retrieves the favicon URI and data URL associated to the given page, if
   * any. If the page icon is not available, it will try to return the root
   * domain icon data, when it's known.
   *
   * @param aPageURI
   *        URI of the page whose favicon URI and data we're looking up.
   * @param [optional] aPreferredWidth
   *        The preferred icon width, skip or pass 0 for the default value,
   *        set through setDefaultIconURIPreferredSize.
   * @param [optional] aOnConcurrentConn
   *        Whether the icon retrieval should be done on ConcurrentConnection
   *        rather than the main DB connection. Defaults to false.
   *
   * @return MozPromise<nsIFavicon, nsresult>
   */
  RefPtr<mozilla::places::FaviconPromise> AsyncGetFaviconForPage(
      nsIURI* aPageURI, uint16_t aPreferredWidth = 0,
      bool aOnConcurrentConn = false);

  /**
   * Try to copy the favicons associated to the aFromURI to aToURI.
   *
   * @param aFromPageURI
   *        URI of the originating page.
   * @param aToPageURI
   *        URI of the destination page.
   * @param aFaviconLoadType
   *        Set to FAVICON_LOAD_PRIVATE if the copy is started from a private
   *        browsing window.  Set to FAVICON_LOAD_NON_PRIVATE otherwise.
   *
   * @return MozPromise<bool, nsresult>
   *         If it could copy, resolved with true.
   */
  RefPtr<mozilla::places::BoolPromise> AsyncTryCopyFavicons(
      nsCOMPtr<nsIURI> aFromPageURI, nsCOMPtr<nsIURI> aToPageURI,
      uint32_t aFaviconLoadType);

  /**
   * Clears the image cache for the given image spec.
   *
   * @param aImageURI
   *        The URI of the image to clear cache for.
   */
  void ClearImageCache(nsIURI* aImageURI);

  static mozilla::Atomic<int64_t> sLastInsertedIconId;
  static void StoreLastInsertedId(const nsACString& aTable,
                                  const int64_t aLastInsertedId);

  NS_DECL_ISUPPORTS
  NS_DECL_NSIFAVICONSERVICE

 private:
  imgITools* GetImgTools() {
    if (!mImgTools) {
      mImgTools = do_CreateInstance("@mozilla.org/image/tools;1");
    }
    return mImgTools;
  }

  ~nsFaviconService();

  RefPtr<mozilla::places::Database> mDB;

  nsCOMPtr<imgITools> mImgTools;

  static nsFaviconService* gFaviconService;

  /**
   * A cached URI for the default icon. We return this a lot, and don't want to
   * re-parse and normalize our unchanging string many times.  Important: do
   * not return this directly; use Clone() since callers may change the object
   * they get back. May be null, in which case it needs initialization.
   */
  nsCOMPtr<nsIURI> mDefaultIcon;
  uint16_t mDefaultIconURIPreferredSize;
};

#endif  // nsFaviconService_h_
