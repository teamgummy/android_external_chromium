// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/dom_ui/history_ui.h"

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/callback.h"
#include "base/i18n/time_formatting.h"
#include "base/message_loop.h"
#include "base/singleton.h"
#include "base/string_piece.h"
#include "base/string_util.h"
#include "base/thread.h"
#include "base/time.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model.h"
#include "chrome/browser/browser.h"
#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/dom_ui/dom_ui_favicon_source.h"
#include "chrome/browser/metrics/user_metrics.h"
#include "chrome/browser/history/history_types.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/browser/tab_contents/tab_contents_delegate.h"
#include "chrome/common/jstemplate_builder.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/time_format.h"
#include "chrome/common/url_constants.h"
#include "net/base/escape.h"

#include "grit/browser_resources.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/locale_settings.h"
#include "grit/theme_resources.h"

// Maximum number of search results to return in a given search. We should
// eventually remove this.
static const int kMaxSearchResults = 100;

////////////////////////////////////////////////////////////////////////////////
//
// HistoryHTMLSource
//
////////////////////////////////////////////////////////////////////////////////

HistoryUIHTMLSource::HistoryUIHTMLSource()
    : DataSource(chrome::kChromeUIHistoryHost, MessageLoop::current()) {
}

void HistoryUIHTMLSource::StartDataRequest(const std::string& path,
                                           bool is_off_the_record,
                                           int request_id) {
  DictionaryValue localized_strings;
  localized_strings.SetString(L"loading",
      l10n_util::GetString(IDS_HISTORY_LOADING));
  localized_strings.SetString(L"title",
      l10n_util::GetString(IDS_HISTORY_TITLE));
  localized_strings.SetString(L"loading",
      l10n_util::GetString(IDS_HISTORY_LOADING));
  localized_strings.SetString(L"newest",
      l10n_util::GetString(IDS_HISTORY_NEWEST));
  localized_strings.SetString(L"newer",
      l10n_util::GetString(IDS_HISTORY_NEWER));
  localized_strings.SetString(L"older",
      l10n_util::GetString(IDS_HISTORY_OLDER));
  localized_strings.SetString(L"searchresultsfor",
      l10n_util::GetString(IDS_HISTORY_SEARCHRESULTSFOR));
  localized_strings.SetString(L"history",
      l10n_util::GetString(IDS_HISTORY_BROWSERESULTS));
  localized_strings.SetString(L"cont",
      l10n_util::GetString(IDS_HISTORY_CONTINUED));
  localized_strings.SetString(L"searchbutton",
      l10n_util::GetString(IDS_HISTORY_SEARCH_BUTTON));
  localized_strings.SetString(L"noresults",
      l10n_util::GetString(IDS_HISTORY_NO_RESULTS));
  localized_strings.SetString(L"noitems",
      l10n_util::GetString(IDS_HISTORY_NO_ITEMS));
  localized_strings.SetString(L"edithistory",
      l10n_util::GetString(IDS_HISTORY_START_EDITING_HISTORY));
  localized_strings.SetString(L"doneediting",
      l10n_util::GetString(IDS_HISTORY_STOP_EDITING_HISTORY));
  localized_strings.SetString(L"removeselected",
      l10n_util::GetString(IDS_HISTORY_REMOVE_SELECTED_ITEMS));
  localized_strings.SetString(L"clearallhistory",
      l10n_util::GetString(IDS_HISTORY_OPEN_CLEAR_BROWSING_DATA_DIALOG));
  localized_strings.SetString(L"deletewarning",
      l10n_util::GetString(IDS_HISTORY_DELETE_PRIOR_VISITS_WARNING));

  SetFontAndTextDirection(&localized_strings);

  static const base::StringPiece history_html(
      ResourceBundle::GetSharedInstance().GetRawDataResource(
          IDR_HISTORY_HTML));
  const std::string full_html = jstemplate_builder::GetI18nTemplateHtml(
      history_html, &localized_strings);

  scoped_refptr<RefCountedBytes> html_bytes(new RefCountedBytes);
  html_bytes->data.resize(full_html.size());
  std::copy(full_html.begin(), full_html.end(), html_bytes->data.begin());

  SendResponse(request_id, html_bytes);
}

////////////////////////////////////////////////////////////////////////////////
//
// HistoryHandler
//
////////////////////////////////////////////////////////////////////////////////
BrowsingHistoryHandler::BrowsingHistoryHandler()
    : search_text_() {
}

BrowsingHistoryHandler::~BrowsingHistoryHandler() {
  cancelable_search_consumer_.CancelAllRequests();
  cancelable_delete_consumer_.CancelAllRequests();
}

DOMMessageHandler* BrowsingHistoryHandler::Attach(DOMUI* dom_ui) {
  // Create our favicon data source.
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      NewRunnableMethod(
          Singleton<ChromeURLDataManager>::get(),
          &ChromeURLDataManager::AddDataSource,
          make_scoped_refptr(new DOMUIFavIconSource(dom_ui->GetProfile()))));

  // Get notifications when history is cleared.
  registrar_.Add(this, NotificationType::HISTORY_URLS_DELETED,
      Source<Profile>(dom_ui->GetProfile()->GetOriginalProfile()));
  return DOMMessageHandler::Attach(dom_ui);
}

void BrowsingHistoryHandler::RegisterMessages() {
  dom_ui_->RegisterMessageCallback("getHistory",
      NewCallback(this, &BrowsingHistoryHandler::HandleGetHistory));
  dom_ui_->RegisterMessageCallback("searchHistory",
      NewCallback(this, &BrowsingHistoryHandler::HandleSearchHistory));
  dom_ui_->RegisterMessageCallback("removeURLsOnOneDay",
      NewCallback(this, &BrowsingHistoryHandler::HandleRemoveURLsOnOneDay));
  dom_ui_->RegisterMessageCallback("clearBrowsingData",
      NewCallback(this, &BrowsingHistoryHandler::HandleClearBrowsingData));
}

void BrowsingHistoryHandler::HandleGetHistory(const Value* value) {
  // Anything in-flight is invalid.
  cancelable_search_consumer_.CancelAllRequests();

  // Get arguments (if any).
  int day = 0;
  ExtractIntegerValue(value, &day);

  // Set our query options.
  history::QueryOptions options;
  options.begin_time = base::Time::Now().LocalMidnight();
  options.begin_time -= base::TimeDelta::FromDays(day);
  options.end_time = base::Time::Now().LocalMidnight();
  options.end_time -= base::TimeDelta::FromDays(day - 1);

  // Need to remember the query string for our results.
  search_text_ = string16();

  HistoryService* hs =
      dom_ui_->GetProfile()->GetHistoryService(Profile::EXPLICIT_ACCESS);
  hs->QueryHistory(search_text_,
      options,
      &cancelable_search_consumer_,
      NewCallback(this, &BrowsingHistoryHandler::QueryComplete));
}

void BrowsingHistoryHandler::HandleSearchHistory(const Value* value) {
  // Anything in-flight is invalid.
  cancelable_search_consumer_.CancelAllRequests();

  // Get arguments (if any).
  int month = 0;
  string16 query;
  ExtractSearchHistoryArguments(value, &month, &query);

  // Set the query ranges for the given month.
  history::QueryOptions options = CreateMonthQueryOptions(month);

  // When searching, limit the number of results returned.
  options.max_count = kMaxSearchResults;

  // Need to remember the query string for our results.
  search_text_ = query;
  HistoryService* hs =
      dom_ui_->GetProfile()->GetHistoryService(Profile::EXPLICIT_ACCESS);
  hs->QueryHistory(search_text_,
      options,
      &cancelable_search_consumer_,
      NewCallback(this, &BrowsingHistoryHandler::QueryComplete));
}

void BrowsingHistoryHandler::HandleRemoveURLsOnOneDay(const Value* value) {
  if (cancelable_delete_consumer_.HasPendingRequests()) {
    dom_ui_->CallJavascriptFunction(L"deleteFailed");
    return;
  }

  DCHECK(value && value->GetType() == Value::TYPE_LIST);

  // Get day to delete data from.
  int visit_time = 0;
  ExtractIntegerValue(value, &visit_time);
  base::Time::Exploded exploded;
  base::Time::FromTimeT(
      static_cast<time_t>(visit_time)).LocalExplode(&exploded);
  exploded.hour = exploded.minute = exploded.second = exploded.millisecond = 0;
  base::Time begin_time = base::Time::FromLocalExploded(exploded);
  base::Time end_time = begin_time + base::TimeDelta::FromDays(1);

  // Get URLs.
  std::set<GURL> urls;
  const ListValue* list_value = static_cast<const ListValue*>(value);
  for (ListValue::const_iterator v = list_value->begin() + 1;
       v != list_value->end(); ++v) {
    if ((*v)->GetType() != Value::TYPE_STRING)
      continue;
    const StringValue* string_value = static_cast<const StringValue*>(*v);
    string16 string16_value;
    if (!string_value->GetAsUTF16(&string16_value))
      continue;
    urls.insert(GURL(string16_value));
  }

  HistoryService* hs =
      dom_ui_->GetProfile()->GetHistoryService(Profile::EXPLICIT_ACCESS);
  hs->ExpireHistoryBetween(
      urls, begin_time, end_time, &cancelable_delete_consumer_,
      NewCallback(this, &BrowsingHistoryHandler::RemoveComplete));
}

void BrowsingHistoryHandler::HandleClearBrowsingData(const Value* value) {
  dom_ui_->tab_contents()->delegate()->GetBrowser()->
      OpenClearBrowsingDataDialog();
}

void BrowsingHistoryHandler::QueryComplete(
    HistoryService::Handle request_handle,
    history::QueryResults* results) {

  ListValue results_value;
  base::Time midnight_today = base::Time::Now().LocalMidnight();

  for (size_t i = 0; i < results->size(); ++i) {
    history::URLResult const &page = (*results)[i];
    DictionaryValue* page_value = new DictionaryValue();
    SetURLAndTitle(page_value, page.title(), page.url());

    // Need to pass the time in epoch time (fastest JS conversion).
    page_value->SetInteger(L"time",
        static_cast<int>(page.visit_time().ToTimeT()));

    // Until we get some JS i18n infrastructure, we also need to
    // pass the dates in as strings. This could use some
    // optimization.

    // Only pass in the strings we need (search results need a shortdate
    // and snippet, browse results need day and time information).
    if (search_text_.empty()) {
      // Figure out the relative date string.
      std::wstring date_str = TimeFormat::RelativeDate(page.visit_time(),
                                                       &midnight_today);
      if (date_str.empty()) {
        date_str = base::TimeFormatFriendlyDate(page.visit_time());
      } else {
        date_str = l10n_util::GetStringF(
            IDS_HISTORY_DATE_WITH_RELATIVE_TIME,
            date_str, base::TimeFormatFriendlyDate(page.visit_time()));
      }
      page_value->SetString(L"dateRelativeDay", date_str);
      page_value->SetString(L"dateTimeOfDay",
          base::TimeFormatTimeOfDay(page.visit_time()));
    } else {
      page_value->SetString(L"dateShort",
          base::TimeFormatShortDate(page.visit_time()));
      page_value->SetStringFromUTF16(L"snippet", page.snippet().text());
    }
    page_value->SetBoolean(L"starred",
        dom_ui_->GetProfile()->GetBookmarkModel()->IsBookmarked(page.url()));
    results_value.Append(page_value);
  }

  DictionaryValue info_value;
  info_value.SetStringFromUTF16(L"term", search_text_);
  info_value.SetBoolean(L"finished", results->reached_beginning());

  dom_ui_->CallJavascriptFunction(L"historyResult", info_value, results_value);
}

void BrowsingHistoryHandler::RemoveComplete() {
  // Some Visits were deleted from history. Reload the list.
  dom_ui_->CallJavascriptFunction(L"deleteComplete");
}

void BrowsingHistoryHandler::ExtractSearchHistoryArguments(const Value* value,
                                                           int* month,
                                                           string16* query) {
  *month = 0;

  if (value && value->GetType() == Value::TYPE_LIST) {
    const ListValue* list_value = static_cast<const ListValue*>(value);
    Value* list_member;

    // Get search string.
    if (list_value->Get(0, &list_member) &&
        list_member->GetType() == Value::TYPE_STRING) {
      const StringValue* string_value =
        static_cast<const StringValue*>(list_member);
      string_value->GetAsUTF16(query);
    }

    // Get search month.
    if (list_value->Get(1, &list_member) &&
        list_member->GetType() == Value::TYPE_STRING) {
      const StringValue* string_value =
        static_cast<const StringValue*>(list_member);
      string16 string16_value;
      string_value->GetAsUTF16(&string16_value);
      *month = StringToInt(string16_value);
    }
  }
}

history::QueryOptions BrowsingHistoryHandler::CreateMonthQueryOptions(
    int month) {
  history::QueryOptions options;

  // Configure the begin point of the search to the start of the
  // current month.
  base::Time::Exploded exploded;
  base::Time::Now().LocalMidnight().LocalExplode(&exploded);
  exploded.day_of_month = 1;

  if (month == 0) {
    options.begin_time = base::Time::FromLocalExploded(exploded);

    // Set the end time of this first search to null (which will
    // show results from the future, should the user's clock have
    // been set incorrectly).
    options.end_time = base::Time();
  } else {
    // Set the end-time of this search to the end of the month that is
    // |depth| months before the search end point. The end time is not
    // inclusive, so we should feel free to set it to midnight on the
    // first day of the following month.
    exploded.month -= month - 1;
    while (exploded.month < 1) {
      exploded.month += 12;
      exploded.year--;
    }
    options.end_time = base::Time::FromLocalExploded(exploded);

    // Set the begin-time of the search to the start of the month
    // that is |depth| months prior to search_start_.
    if (exploded.month > 1) {
      exploded.month--;
    } else {
      exploded.month = 12;
      exploded.year--;
    }
    options.begin_time = base::Time::FromLocalExploded(exploded);
  }

  return options;
}

void BrowsingHistoryHandler::Observe(NotificationType type,
                                     const NotificationSource& source,
                                     const NotificationDetails& details) {
  if (type != NotificationType::HISTORY_URLS_DELETED) {
    NOTREACHED();
    return;
  }

  // Some URLs were deleted from history. Reload the list.
  dom_ui_->CallJavascriptFunction(L"historyDeleted");
}

////////////////////////////////////////////////////////////////////////////////
//
// HistoryUIContents
//
////////////////////////////////////////////////////////////////////////////////

HistoryUI::HistoryUI(TabContents* contents) : DOMUI(contents) {
  AddMessageHandler((new BrowsingHistoryHandler())->Attach(this));

  HistoryUIHTMLSource* html_source = new HistoryUIHTMLSource();

  // Set up the chrome://history/ source.
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      NewRunnableMethod(
          Singleton<ChromeURLDataManager>::get(),
          &ChromeURLDataManager::AddDataSource,
          make_scoped_refptr(html_source)));
}

// static
const GURL HistoryUI::GetHistoryURLWithSearchText(const string16& text) {
  return GURL(std::string(chrome::kChromeUIHistoryURL) + "#q=" +
              EscapeQueryParamValue(UTF16ToUTF8(text), true));
}

// static
RefCountedMemory* HistoryUI::GetFaviconResourceBytes() {
  return ResourceBundle::GetSharedInstance().
      LoadDataResourceBytes(IDR_HISTORY_FAVICON);
}