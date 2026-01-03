/**
 * Simple Gopher Browser for PocketBook 622
 *
 * A basic Gopher protocol (RFC 1436) browser for e-ink devices.
 *
 * Touch Controls:
 *   Tap item         - Select item
 *   Double-tap item  - Follow link
 *   Swipe up/down    - Scroll content
 *   Tap header       - Show bookmarks menu
 *
 * Hardware Keys:
 *   KEY_NEXT (Right) - Follow selected link
 *   KEY_PREV (Left)  - Go back in history
 */

#include "inkview.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <vector>
#include <string>
#include <cstring>

// ============================================================================
// Constants
// ============================================================================

static const int kFontSize = 14;
static const int kTitleFontSize = 20;
static const int kLineHeight = kFontSize + 4;
static const int kMaxHistory = 50;
static const int kSocketTimeout = 15;
static const int kDefaultGopherPort = 70;
static const int kMaxResponseSize = 512 * 1024; // 512KB max response
static const int kScreenMargin = 1;             // Screen edge margin
static const int kDoubleTapTime = 500;          // Double-tap threshold in ms
// static const int kContentPadding = 4;        // Padding inside content area

// Default starting page - Floodgap's Gopher server
static const char *kDefaultHost = "gopher.floodgap.com";
static const char *kDefaultSelector = "/";

// Gopher item types (RFC 1436)
enum GopherItemType
{
    GOPHER_TEXT = '0',      // Text file
    GOPHER_MENU = '1',      // Gopher menu
    GOPHER_CSO = '2',       // CSO phone-book server
    GOPHER_ERROR = '3',     // Error
    GOPHER_BINHEX = '4',    // BinHex encoded file
    GOPHER_DOS = '5',       // DOS binary archive
    GOPHER_UUENCODE = '6',  // UUEncoded file
    GOPHER_SEARCH = '7',    // Search engine
    GOPHER_TELNET = '8',    // Telnet session
    GOPHER_BINARY = '9',    // Binary file
    GOPHER_REDUNDANT = '+', // Redundant server
    GOPHER_TN3270 = 'T',    // TN3270 session
    GOPHER_GIF = 'g',       // GIF image
    GOPHER_IMAGE = 'I',     // Image (other)
    GOPHER_INFO = 'i',      // Info line (not selectable)
    GOPHER_HTML = 'h',      // HTML file
    GOPHER_SOUND = 's',     // Sound file
    GOPHER_DOC = 'd',       // Document
};

// ============================================================================
// Data Structures
// ============================================================================

struct GopherItem
{
    char type;
    std::string display;
    std::string selector;
    std::string host;
    int port;

    bool is_selectable() const
    {
        return type != GOPHER_INFO && type != GOPHER_ERROR &&
               type != '+' && type != GOPHER_CSO &&
               type != GOPHER_TELNET && type != GOPHER_TN3270;
    }
};

struct GopherPage
{
    std::string host;
    std::string selector;
    int port;
    std::vector<GopherItem> items;
    std::string raw_text; // For text files
    bool is_menu;
};

struct HistoryEntry
{
    std::string host;
    std::string selector;
    int port;
};

// ============================================================================
// Application State
// ============================================================================

static ifont *mono_font = NULL;

static GopherPage current_page;
static std::vector<HistoryEntry> history;

static int scroll_offset = 0;       // Current scroll position (in lines)
static int selected_index = -1;     // Currently selected item index
static int visible_lines = 0;       // Number of lines visible on screen
static int header_height = 0;       // Height of header area
static int content_area_top = 0;    // Top of content area
static int content_area_bottom = 0; // Bottom of content area

static bool is_loading = false;
static char status_message[256] = {0};

// Search input state
static char search_query[256] = {0};   // Buffer for search input
static GopherItem pending_search_item; // Item being searched
static bool search_pending = false;    // Whether a search is pending

static bool initial_load_done = false; // Whether initial page has been loaded

// Touch/gesture tracking
static int last_tap_index = -1;    // Last tapped item index
static long last_tap_time = 0;     // Time of last tap (for double-tap detection)
static int touch_start_y = 0;      // Y position at touch start (for swipe detection)
static bool touch_is_drag = false; // Whether current touch is a drag/swipe

// ============================================================================
// Utility Functions
// ============================================================================

static long get_current_time_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static void set_status(const char *msg)
{
    strncpy(status_message, msg, sizeof(status_message) - 1);
    status_message[sizeof(status_message) - 1] = '\0';
}

static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> split(const std::string &s, char delim)
{
    std::vector<std::string> result;
    std::string token;
    for (size_t i = 0; i < s.length(); i++)
    {
        if (s[i] == delim)
        {
            result.push_back(token);
            token.clear();
        }
        else
        {
            token += s[i];
        }
    }
    result.push_back(token);
    return result;
}

// ============================================================================
// Network Functions
// ============================================================================

static int connect_to_host(const char *hostname, int port)
{
    struct hostent *he;
    struct sockaddr_in server_addr;
    int sockfd;

    // Resolve hostname
    he = gethostbyname(hostname);
    if (he == NULL)
    {
        set_status("DNS resolution failed");
        return -1;
    }

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        set_status("Failed to create socket");
        return -1;
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = kSocketTimeout;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    // Connect
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        close(sockfd);
        set_status("Connection failed");
        return -1;
    }

    return sockfd;
}

static std::string fetch_gopher(const char *host, const char *selector, int port)
{
    char buffer[4096];
    std::string response;

    int sockfd = connect_to_host(host, port);
    if (sockfd < 0)
    {
        return "";
    }

    // Send selector + CRLF
    std::string request = std::string(selector) + "\r\n";
    if (send(sockfd, request.c_str(), request.length(), 0) < 0)
    {
        close(sockfd);
        set_status("Failed to send request");
        return "";
    }

    // Receive response
    ssize_t bytes_received;
    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0)
    {
        buffer[bytes_received] = '\0';
        response += buffer;

        // Safety limit
        if (response.length() > kMaxResponseSize)
        {
            set_status("Response too large");
            break;
        }
    }

    close(sockfd);
    return response;
}

// ============================================================================
// Gopher Protocol Parsing
// ============================================================================

static GopherItem parse_gopher_line(const std::string &line)
{
    GopherItem item;
    item.port = kDefaultGopherPort;

    if (line.empty())
    {
        item.type = GOPHER_INFO;
        item.display = "";
        return item;
    }

    // First character is the type
    item.type = line[0];

    // Rest is tab-separated: display\tselector\thost\tport
    std::string rest = line.substr(1);
    std::vector<std::string> parts = split(rest, '\t');

    if (parts.size() >= 1)
    {
        item.display = parts[0];
    }
    if (parts.size() >= 2)
    {
        item.selector = parts[1];
    }
    if (parts.size() >= 3)
    {
        item.host = parts[2];
    }
    if (parts.size() >= 4)
    {
        std::string port_str = trim(parts[3]);
        if (!port_str.empty())
        {
            item.port = atoi(port_str.c_str());
            if (item.port <= 0)
                item.port = kDefaultGopherPort;
        }
    }

    return item;
}

static void parse_gopher_menu(const std::string &response, GopherPage &page)
{
    page.items.clear();
    page.is_menu = true;

    std::string line;
    for (size_t i = 0; i < response.length(); i++)
    {
        char c = response[i];
        if (c == '\n')
        {
            // Remove trailing CR if present
            if (!line.empty() && line[line.length() - 1] == '\r')
            {
                line = line.substr(0, line.length() - 1);
            }

            // Check for end marker
            if (line == ".")
            {
                break;
            }

            if (!line.empty())
            {
                page.items.push_back(parse_gopher_line(line));
            }
            line.clear();
        }
        else
        {
            line += c;
        }
    }

    // Handle last line if no newline at end
    if (!line.empty() && line != ".")
    {
        page.items.push_back(parse_gopher_line(line));
    }
}

static void parse_text_file(const std::string &response, GopherPage &page)
{
    page.items.clear();
    page.is_menu = false;
    page.raw_text = response;

    // Convert text to info items for display
    std::string line;
    for (size_t i = 0; i < response.length(); i++)
    {
        char c = response[i];
        if (c == '\n')
        {
            if (!line.empty() && line[line.length() - 1] == '\r')
            {
                line = line.substr(0, line.length() - 1);
            }
            if (line == ".")
            {
                break;
            }

            GopherItem item;
            item.type = GOPHER_INFO;
            item.display = line;
            page.items.push_back(item);
            line.clear();
        }
        else
        {
            line += c;
        }
    }

    if (!line.empty() && line != ".")
    {
        GopherItem item;
        item.type = GOPHER_INFO;
        item.display = line;
        page.items.push_back(item);
    }
}

// ============================================================================
// Navigation
// ============================================================================

static void navigate_to(const char *host, const char *selector, int port, char expected_type = GOPHER_MENU);
static void draw_screen();

static void push_history()
{
    HistoryEntry entry;
    entry.host = current_page.host;
    entry.selector = current_page.selector;
    entry.port = current_page.port;

    history.push_back(entry);

    // Limit history size
    while (history.size() > kMaxHistory)
    {
        history.erase(history.begin());
    }
}

static bool go_back()
{
    if (history.empty())
    {
        return false;
    }

    HistoryEntry entry = history.back();
    history.pop_back();

    // Navigate without adding to history
    current_page.host = entry.host;
    current_page.selector = entry.selector;
    current_page.port = entry.port;

    set_status("Loading...");
    is_loading = true;

    std::string response = fetch_gopher(entry.host.c_str(), entry.selector.c_str(), entry.port);

    is_loading = false;

    if (response.empty())
    {
        set_status("Failed to load page");
        return false;
    }

    parse_gopher_menu(response, current_page);
    scroll_offset = 0;
    selected_index = -1;

    // Find first selectable item
    for (size_t i = 0; i < current_page.items.size(); i++)
    {
        if (current_page.items[i].is_selectable())
        {
            selected_index = i;
            break;
        }
    }

    set_status("");
    return true;
}

static void navigate_to(const char *host, const char *selector, int port, char expected_type)
{
    // Save current page to history
    if (!current_page.host.empty())
    {
        push_history();
    }

    current_page.host = host;
    current_page.selector = selector;
    current_page.port = port;

    set_status("Connecting...");
    is_loading = true;

    std::string response = fetch_gopher(host, selector, port);

    is_loading = false;

    if (response.empty())
    {
        set_status("Failed to load page");
        return;
    }

    // Parse based on expected type
    if (expected_type == GOPHER_TEXT || expected_type == GOPHER_HTML)
    {
        parse_text_file(response, current_page);
    }
    else
    {
        parse_gopher_menu(response, current_page);
    }

    scroll_offset = 0;
    selected_index = -1;

    // Find first selectable item
    for (size_t i = 0; i < current_page.items.size(); i++)
    {
        if (current_page.items[i].is_selectable())
        {
            selected_index = i;
            break;
        }
    }

    set_status("");
}

// Keyboard handler for search input
static void search_keyboard_handler(char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        // User cancelled or entered empty string
        search_pending = false;
        draw_screen();
        return;
    }

    // Build the search selector: original_selector + TAB + query
    std::string search_selector = pending_search_item.selector + "\t" + text;

    // Navigate to the search results
    navigate_to(pending_search_item.host.c_str(),
                search_selector.c_str(),
                pending_search_item.port,
                GOPHER_MENU);

    search_pending = false;
    draw_screen();
}

static void initiate_search(const GopherItem &item)
{
    // Store the item we're searching
    pending_search_item = item;
    search_pending = true;

    // Clear the search buffer
    memset(search_query, 0, sizeof(search_query));

    // Open the keyboard for input
    // Title shows what we're searching
    char title[128];
    snprintf(title, sizeof(title), "Search: %s", item.display.c_str());

    OpenKeyboard(title, search_query, sizeof(search_query) - 1,
                 KBD_NORMAL, search_keyboard_handler);
}

static void follow_link()
{
    if (selected_index < 0 || selected_index >= (int)current_page.items.size())
    {
        return;
    }

    const GopherItem &item = current_page.items[selected_index];

    if (!item.is_selectable())
    {
        return;
    }

    // Handle different item types
    switch (item.type)
    {
    case GOPHER_MENU:
        navigate_to(item.host.c_str(), item.selector.c_str(), item.port, GOPHER_MENU);
        break;

    case GOPHER_SEARCH:
        // Open keyboard for search query input
        initiate_search(item);
        break;

    case GOPHER_TEXT:
    case GOPHER_HTML:
        navigate_to(item.host.c_str(), item.selector.c_str(), item.port, GOPHER_TEXT);
        break;

    case GOPHER_BINARY:
    case GOPHER_IMAGE:
    case GOPHER_GIF:
    case GOPHER_SOUND:
    case GOPHER_DOS:
    case GOPHER_BINHEX:
    case GOPHER_UUENCODE:
        Message(ICON_WARNING, "Gopher Browser",
                "Binary files cannot be displayed", 2000);
        break;

    default:
        // Try as menu
        navigate_to(item.host.c_str(), item.selector.c_str(), item.port, GOPHER_MENU);
        break;
    }
}

// ============================================================================
// Display Functions
// ============================================================================

static const char *get_type_prefix(char type)
{
    switch (type)
    {
    case GOPHER_TEXT:
        return "[T]";
    case GOPHER_MENU:
        return "[D]";
    case GOPHER_SEARCH:
        return "[?]";
    case GOPHER_BINARY:
        return "[B]";
    case GOPHER_IMAGE:
    case GOPHER_GIF:
        return "[I]";
    case GOPHER_SOUND:
        return "[S]";
    case GOPHER_HTML:
        return "[H]";
    case GOPHER_ERROR:
        return "[E]";
    case GOPHER_INFO:
        return "   ";
    default:
        return "[?]";
    }
}

static void draw_screen()
{
    ClearScreen();

    int screen_width = ScreenWidth();
    int screen_height = ScreenHeight();
    int content_width = screen_width - (kScreenMargin * 2);
    int y = kScreenMargin;

    // Draw header
    SetFont(mono_font, BLACK);

    char header[256];
    snprintf(header, sizeof(header), "Gopher: %s", current_page.host.c_str());
    DrawTextRect(kScreenMargin + 6, y, content_width - 12, kTitleFontSize, header, ALIGN_LEFT);
    y += kTitleFontSize + 2;

    // Draw current path
    SetFont(mono_font, DGRAY);
    DrawTextRect(kScreenMargin + 6, y, content_width - 12, kFontSize,
                 current_page.selector.c_str(), ALIGN_LEFT);
    y += kFontSize + 2;

    // Separator line
    DrawLine(kScreenMargin, y, screen_width - kScreenMargin, y, BLACK);
    y += 4;

    header_height = y;
    content_area_top = y;

    // Calculate visible lines (leave space for footer)
    int footer_height = 30;
    visible_lines = (screen_height - header_height - footer_height - kScreenMargin) / kLineHeight;
    content_area_bottom = header_height + (visible_lines * kLineHeight);

    // Draw items
    SetFont(mono_font, BLACK);

    int items_count = current_page.items.size();
    int end_index = scroll_offset + visible_lines;
    if (end_index > items_count)
        end_index = items_count;

    for (int i = scroll_offset; i < end_index; i++)
    {
        const GopherItem &item = current_page.items[i];

        // Highlight selected item
        if (i == selected_index)
        {
            FillArea(kScreenMargin, y, content_width, kLineHeight, LGRAY);
        }

        // Draw type prefix
        const char *prefix = get_type_prefix(item.type);
        SetFont(mono_font, (item.type == GOPHER_INFO) ? DGRAY : BLACK);
        DrawTextRect(kScreenMargin, y + 2, kScreenMargin + 28, kFontSize, prefix, ALIGN_LEFT);

        // Draw display text
        SetFont(mono_font, BLACK);

        // Truncate long lines
        char display_buf[256];
        int max_chars = (content_width - 38) / 8; // Approximate char width
        if (max_chars > 255)
            max_chars = 255;

        strncpy(display_buf, item.display.c_str(), max_chars);
        display_buf[max_chars] = '\0';

        DrawTextRect(kScreenMargin + 24, y + 2, content_width - 8, kFontSize, display_buf, ALIGN_LEFT);

        y += kLineHeight;
    }

    // Draw scrollbar if needed
    if (items_count > visible_lines)
    {
        int scrollbar_height = content_area_bottom - header_height;
        int thumb_height = (visible_lines * scrollbar_height) / items_count;
        if (thumb_height < 20)
            thumb_height = 20;

        int thumb_pos = header_height + (scroll_offset * scrollbar_height) / items_count;

        // Scrollbar track
        FillArea(screen_width - kScreenMargin - 6, header_height, 5, scrollbar_height, LGRAY);
        // Scrollbar thumb
        FillArea(screen_width - kScreenMargin - 6, thumb_pos, 5, thumb_height, DGRAY);
    }

    // Draw footer/status bar
    y = screen_height - 25 - kScreenMargin;
    DrawLine(kScreenMargin, y, screen_width - kScreenMargin, y, BLACK);
    y += 5;

    SetFont(mono_font, DGRAY);

    if (status_message[0] != '\0')
    {
        DrawTextRect(kScreenMargin + 6, y, content_width - 120, kFontSize, status_message, ALIGN_LEFT);
    }

    // Page indicator and hint
    char page_info[64];
    int current_page_num = (scroll_offset / visible_lines) + 1;
    int total_pages = ((items_count + visible_lines - 1) / visible_lines);
    if (total_pages < 1)
        total_pages = 1;

    snprintf(page_info, sizeof(page_info), "%d/%d", current_page_num, total_pages);
    DrawTextRect(screen_width - kScreenMargin - 100, y, 94, kFontSize, page_info, ALIGN_RIGHT);

    FullUpdate();
}

// ============================================================================
// Input Handling
// ============================================================================

static void move_selection(int direction)
{
    int items_count = current_page.items.size();
    if (items_count == 0)
        return;

    int new_index = selected_index + direction;

    // Find next selectable item
    while (new_index >= 0 && new_index < items_count)
    {
        if (current_page.items[new_index].is_selectable())
        {
            selected_index = new_index;

            // Adjust scroll if needed
            if (selected_index < scroll_offset)
            {
                scroll_offset = selected_index;
            }
            else if (selected_index >= scroll_offset + visible_lines)
            {
                scroll_offset = selected_index - visible_lines + 1;
            }

            draw_screen();
            return;
        }
        new_index += direction;
    }

    // If no selectable item found in direction, try wrapping
    if (direction > 0)
    {
        // Try from start
        for (int i = 0; i < selected_index; i++)
        {
            if (current_page.items[i].is_selectable())
            {
                selected_index = i;
                scroll_offset = 0;
                draw_screen();
                return;
            }
        }
    }
    else
    {
        // Try from end
        for (int i = items_count - 1; i > selected_index; i--)
        {
            if (current_page.items[i].is_selectable())
            {
                selected_index = i;
                scroll_offset = selected_index - visible_lines + 1;
                if (scroll_offset < 0)
                    scroll_offset = 0;
                draw_screen();
                return;
            }
        }
    }
}

static void scroll_page(int direction)
{
    int items_count = current_page.items.size();
    int max_scroll = items_count - visible_lines;
    if (max_scroll < 0)
        max_scroll = 0;

    scroll_offset += direction * visible_lines;

    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;

    draw_screen();
}

static void bookmark_menu_handler(int index)
{
    switch (index)
    {
    case 0:
        navigate_to("gopher.floodgap.com", "/", 70);
        break;
    case 1:
        navigate_to("sdf.org", "/", 70);
        break;
    case 2:
        navigate_to("gopherpedia.com", "/", 70);
        break;
    case 3:
        navigate_to("gopher.floodgap.com", "/v2/vs", 70);
        break;
    }
    draw_screen();
}

static void show_bookmarks_menu()
{
    static imenu bookmark_items[5];

    bookmark_items[0].type = ITEM_ACTIVE;
    bookmark_items[0].index = 0;
    bookmark_items[0].text = (char *)"Floodgap Gopher";
    bookmark_items[0].submenu = NULL;

    bookmark_items[1].type = ITEM_ACTIVE;
    bookmark_items[1].index = 1;
    bookmark_items[1].text = (char *)"SDF Public Access";
    bookmark_items[1].submenu = NULL;

    bookmark_items[2].type = ITEM_ACTIVE;
    bookmark_items[2].index = 2;
    bookmark_items[2].text = (char *)"Gopherpedia";
    bookmark_items[2].submenu = NULL;

    bookmark_items[3].type = ITEM_ACTIVE;
    bookmark_items[3].index = 3;
    bookmark_items[3].text = (char *)"Veronica-2 Search";
    bookmark_items[3].submenu = NULL;

    bookmark_items[4].type = 0;
    bookmark_items[4].index = 0;
    bookmark_items[4].text = NULL;
    bookmark_items[4].submenu = NULL;

    OpenMenu(bookmark_items, 0, 50, 100, (iv_menuhandler)bookmark_menu_handler);
}

static void handle_key(int key)
{
    switch (key)
    {
    case KEY_LEFT:
    case KEY_PREV:
        if (!go_back())
        {
            Message(ICON_INFORMATION, "Gopher Browser",
                    "No more history", 1500);
        }
        draw_screen();
        break;

    case KEY_RIGHT:
    case KEY_NEXT:
        follow_link();
        if (!search_pending)
        {
            draw_screen();
        }
        break;

    case KEY_MENU:
        show_bookmarks_menu();
        break;

    case KEY_BACK:
        CloseApp();
        break;

    default:
        break;
    }
}

// ============================================================================
// Main Handler
// ============================================================================

static int main_handler(int event_type, int param_one, int param_two)
{
    int result = 0;

    switch (event_type)
    {
    case EVT_INIT:
        // Initialize font
        // mono_font = OpenFont("LiberationMono", kFontSize, 0);
        mono_font = OpenFont("DroidSansMono", kFontSize, 1);
        SetFont(mono_font, BLACK);

        ClearScreen();
        FullUpdate();
        break;

    case EVT_SHOW:
        // Load initial page only on first show
        if (!initial_load_done)
        {
            navigate_to(kDefaultHost, kDefaultSelector, kDefaultGopherPort);
            initial_load_done = true;
        }
        draw_screen();
        break;

    case EVT_KEYPRESS:
        handle_key(param_one);
        result = 1;
        break;

    case EVT_POINTERDOWN:
        // Record touch start position for swipe detection
        touch_start_y = param_two;
        touch_is_drag = false;
        result = 1;
        break;

    case EVT_POINTERMOVE:
        // Detect drag/swipe
        {
            int delta_y = param_two - touch_start_y;
            if (delta_y > 20 || delta_y < -20)
            {
                touch_is_drag = true;
            }
        }
        result = 1;
        break;

    case EVT_POINTERUP:
    {
        int touch_x = param_one;
        int touch_y = param_two;
        int delta_y = touch_y - touch_start_y;

        // Check if this was a swipe gesture
        if (touch_is_drag)
        {
            // Swipe up = scroll down, swipe down = scroll up
            int swipe_lines = -delta_y / kLineHeight;
            if (swipe_lines != 0)
            {
                int items_count = current_page.items.size();
                int max_scroll = items_count - visible_lines;
                if (max_scroll < 0)
                    max_scroll = 0;

                scroll_offset += swipe_lines;
                if (scroll_offset < 0)
                    scroll_offset = 0;
                if (scroll_offset > max_scroll)
                    scroll_offset = max_scroll;

                draw_screen();
            }
        }
        else
        {
            // This was a tap
            long current_time = get_current_time_ms();

            // Check if tap is in header area -> show menu
            if (touch_y < header_height)
            {
                show_bookmarks_menu();
            }
            // Check if tap is in content area
            else if (touch_y >= content_area_top && touch_y < content_area_bottom)
            {
                int line_index = scroll_offset + (touch_y - content_area_top) / kLineHeight;
                if (line_index >= 0 && line_index < (int)current_page.items.size())
                {
                    const GopherItem &item = current_page.items[line_index];

                    if (item.is_selectable())
                    {
                        // Check for double-tap on same item
                        if (line_index == last_tap_index &&
                            (current_time - last_tap_time) < kDoubleTapTime)
                        {
                            // Double-tap: follow link
                            selected_index = line_index;
                            follow_link();
                            if (!search_pending)
                            {
                                draw_screen();
                            }
                            last_tap_index = -1;
                            last_tap_time = 0;
                        }
                        else
                        {
                            // Single tap: select item
                            selected_index = line_index;
                            last_tap_index = line_index;
                            last_tap_time = current_time;
                            draw_screen();
                        }
                    }
                }
            }
        }
    }
        result = 1;
        break;

    case EVT_EXIT:
        // Cleanup
        if (mono_font)
            CloseFont(mono_font);

        history.clear();
        current_page.items.clear();
        break;

    default:
        break;
    }

    return result;
}

// ============================================================================
// Entry Point
// ============================================================================

int main(int argc, char *argv[])
{
    InkViewMain(main_handler);
    return 0;
}
