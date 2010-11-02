// This program reads all the issues in the issues directory passed as the first command line argument.
// If all documents are successfully parsed, it will generate the standard LWG Issues List  documents
// for an ISO SC22 WG21 mailing.

// Based on code originally donated by Howard Hinnant
// Since modified by Alisdair Meredith

// Note that this program rquires a reasonably conformant C++0x compiler
// The only known compiler to support this today is the experimental gcc trunk (4.6)

// standard headers
#include <sstream>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iterator>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cassert>

// platform headers - requires a Posix compatible platform
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

// solution specific headers
#include "date.h"

namespace greg = gregorian;

// Production code
std::string const insert_color{"ins {background-color:#A0FFA0}\n"};
std::string const delete_color{"del {background-color:#FFA0A0}\n"};


// Functions to "normalize" a status string
auto remove_pending(std::string stat) -> std::string {
   typedef std::string::size_type size_type;
   if(0 == stat.find("Pending")) {
      stat.erase(size_type{0}, size_type{8});
   }
   return stat;
}

auto remove_tentatively(std::string stat) -> std::string {
   typedef std::string::size_type size_type;
   if(0 == stat.find("Tentatively")) {
      stat.erase(size_type{0}, size_type{12});
   }
   return stat;
}

auto remove_qualifier(std::string const & stat) -> std::string {
   return remove_tentatively(remove_pending(stat));
}


// functions to relate the status of an issue to its relevant published list document
auto find_file(std::string stat) -> std::string {
   // Tentative issues are always active
   if(0 == stat.find("Tentatively")) {
      return "lwg-active.html";
   }

   stat = remove_qualifier(stat);
   return (stat == "TC1")           ? "lwg-defects.html"
        : (stat == "CD1")           ? "lwg-defects.html"
        : (stat == "WP")            ? "lwg-defects.html"
        : (stat == "DR")            ? "lwg-defects.html"
        : (stat == "TRDec")         ? "lwg-defects.html"
        : (stat == "Dup")           ? "lwg-closed.html"
        : (stat == "NAD")           ? "lwg-closed.html"
        : (stat == "NAD Future")    ? "lwg-closed.html"
        : (stat == "NAD Editorial") ? "lwg-closed.html"
        : (stat == "NAD Concepts")  ? "lwg-closed.html"
        : (stat == "Ready")         ? "lwg-active.html"
        : (stat == "Review")        ? "lwg-active.html"
        : (stat == "New")           ? "lwg-active.html"
        : (stat == "Open")          ? "lwg-active.html"
        : throw std::runtime_error("unknown status " + stat);
}

auto is_active_not_ready(const std::string& stat) -> bool {
   return find_file(stat) == "lwg-active.html"  and  stat != "Ready";
}

auto is_active(const std::string& stat) -> bool {
   return find_file(stat) == "lwg-active.html";
}

auto is_defect(const std::string& stat) -> bool {
   return find_file(stat) == "lwg-defects.html";
}

auto is_closed(const std::string& stat) -> bool {
   return find_file(stat) == "lwg-closed.html";
}

auto is_tentative(const std::string& stat) -> bool {
   // a more efficient implementation will use some variation of strcmp
   return 0 == stat.find("Tentatively");
}

auto is_not_resolved(const std::string& stat) -> bool {
   for( auto s : {"New", "Open", "Review"}) { if(s == stat) return true; }
   return false;
}

struct section_num {
   std::string       prefix;
   std::vector<int>  num;
};


auto operator >> (std::istream& is, section_num& sn) -> std::istream & {
   sn.prefix.clear();
   sn.num.clear();
   ws(is);
   if (is.peek() == 'T') {
      is.get();
      if (is.peek() == 'R') {
         std::string w;
         is >> w;
         if (w == "R1") {
            sn.prefix = "TR1";
         }
         else if (w == "RDecimal") {
            sn.prefix = "TRDecimal";
         }
         else {
            throw std::runtime_error{"section_num format error"};
         }
         ws(is);
      }
      else {
         sn.num.push_back(100 + 'T' - 'A');
         if (is.peek() != '.') {
            return is;
         }
         is.get();
      }
   }

   while (true) {
      if (std::isdigit(is.peek())) {
         int n;
         is >> n;
         sn.num.push_back(n);
      }
      else {
         char c;
         is >> c;
         sn.num.push_back(100 + c - 'A');
      }
      if (is.peek() != '.') {
         break;
      }
      char dot;
      is >> dot;
   }
   return is;
}

auto operator << (std::ostream& os, const section_num& sn) -> std::ostream & {
   if (!sn.prefix.empty()) { os << sn.prefix << " "; }

   bool use_period{false};
   for (auto sub : sn.num ) {
      if(use_period++) {
         os << '.';
      }

      if (sub >= 100) {
         os << char{sub - 100 + 'A'};
      }
      else {
         os << sub;
      }
   }
   return os;
}

auto operator<(const section_num& x, const section_num& y) noexcept -> bool {
   return (x.prefix < y.prefix) ?  true
        : (y.prefix < x.prefix) ? false
        : x.num < y.num;
}

auto operator==(const section_num& x, const section_num& y) noexcept -> bool {
   return (x.prefix != y.prefix)
        ? false
        : x.num == y.num;
}

auto operator!=(const section_num& x, const section_num& y) noexcept -> bool {
   return !(x == y);
}

typedef std::string section_tag;


struct issue   {
   int num;
   std::string                stat;
   std::string                title;
   std::vector<section_tag>   tags;
   std::string                submitter;
   greg::date                 date;
   greg::date                 mod_date;
   std::vector<std::string>   duplicates;
   std::string                text;
   bool                       has_resolution;

   // struggling with implicitly declared move operations in GCC 4.6 development compiler
   issue() = default;
   issue(issue const &) = default;
   auto operator=(issue const &) -> issue & = default;
};


typedef std::map<section_tag, section_num> SectionMap;
SectionMap section_db;


auto remove_square_brackets(section_tag const & tag) -> section_tag {
   assert(tag.size() > 2);
   assert(tag[0] == '[');
   assert(tag[tag.size()-1] == ']');
   return tag.substr(1, tag.size()-2);
}


struct sort_by_section {
    auto operator()(issue const & x, issue const & y) const -> bool {
assert(!x.tags.empty());
assert(!y.tags.empty());
        return section_db[x.tags.front()] < section_db[y.tags.front()];
    }
};


struct sort_by_num {
    bool operator()(const issue& x, const issue& y) const noexcept   {  return x.num < y.num;   }
    bool operator()(const issue& x, int y)          const noexcept   {  return x.num < y;       }
    bool operator()(int x,          const issue& y) const noexcept   {  return x     < y.num;   }
};


struct sort_by_status {
   auto operator()(const issue& x, const issue& y) const noexcept -> bool {
      static auto get_priority = []( std::string const & stat ) -> unsigned {
         static char const * const status_priority[] {
            "Ready",
            "Tentatively Ready",
            "Tentatively NAD Editorial",
            "Tentatively NAD Future",
            "Tentatively NAD",
            "Review",
            "New",
            "Open",
            "NAD Future",
            "NAD Editorial",
            "WP",
            "CD1",
            "TC1",
            "TRDec",
            "Dups",
            "NAD",
            "NAD Concepts"
         };
         // Don't know why gcc 4.6 rejects this - cannot deduce iterators for 'find_if' algorithm
         //static auto first = std::begin(status_priority);
         //static auto last  = std::end(status_priority);
         //return std::find_if( first, last), [&](char const * str){ return str == stat; } ) - first;

         // Yet gcc 4.6 does work with this that should have identical semantics, other than an lvalue/rvalue switch
         return std::find_if( std::begin(status_priority), std::end(status_priority), [&](char const * str){ return str == stat; } ) - std::begin(status_priority);
      };

      return get_priority(x.stat) < get_priority(y.stat);
   }
};


struct sort_by_first_tag {
   bool operator()(const issue& x, const issue& y) const noexcept {
assert(!x.tags.empty());
assert(!y.tags.empty());
      return x.tags.front() < y.tags.front();
   }
};


struct equal_issue_num {
    bool operator()(const issue& x, int y) const noexcept {return x.num == y;}
    bool operator()(int x, const issue& y) const noexcept {return x == y.num;}
};

/*
void display(const std::vector<issue>& issues)
{
    for (auto const & i : issues) { std::cout << i; }
}
*/


auto read_file_into_string(std::string const & filename) -> std::string {
   std::ifstream infile{filename.c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Unable to open file " + filename};
   }

   std::istreambuf_iterator<char> first{infile}, last{};
   return std::string {first, last};
}


auto read_section_db(std::string const & path) -> SectionMap {
   auto filename = path + "section.data";
   std::ifstream infile{filename.c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Can't open section.data\n"};
   }
   std::cout << "Reading section-tag index from: " << filename << std::endl;

   SectionMap section_db;
   while (infile) {
      ws(infile);
      std::string line;
      getline(infile, line);
      if (!line.empty()) {
         assert(line.back() == ']');
         auto p = line.rfind('[');
         assert(p != std::string::npos);
         section_tag tag = line.substr(p);
         assert(tag.size() > 2);
         assert(tag[0] == '[');
         assert(tag[tag.size()-1] == ']');
         line.erase(p-1);

         section_num num;
         if (tag.find("[trdec.") != std::string::npos) {
            num.prefix = "TRDecimal";
            line.erase(0, 10);
         }
         else if (tag.find("[tr.") != std::string::npos) {
            num.prefix = "TR1";
            line.erase(0, 4);
         }

         std::istringstream temp(line);
         if (!std::isdigit(line[0])) {
            char c;
            temp >> c;
            num.num.push_back(100 + c - 'A');
            temp >> c;
         }

         while (temp) {
            int n;
            temp >> n;
            if (!temp.fail()) {
               num.num.push_back(n);
               char dot;
               temp >> dot;
            }
         }

         section_db[tag] = num;
      }
   }
   return section_db;
}


void check_against_index(const SectionMap& section_db) {
   for (auto const & elem : section_db ) {
      std::string temp = elem.first;
      temp.erase(temp.end()-1);
      temp.erase(temp.begin());
      std::cout << temp << ' ' << elem.second << '\n';
   }
}


auto get_date() -> std::string {
#if 1
   greg::date today;
   std::ostringstream date;
   date << today.year() << '-';
   if (today.month() < 10) {
      date << '0';
   }
   date << today.month() << '-';
   if (today.day() < 10) {
      date << '0';
   }
   date << today.day();
   return date.str();
#else
    std::ifstream infile((issues_path + "lwg-issues.xml").c_str());
    if (!infile.is_open()) {
        throw std::runtime_error("Unable to open lwg-issues.xml");
    }
    std::istreambuf_iterator<char> first(infile), last;
    std::string const & s = load_lwg_issues_xml();
    auto i = s.find("date=\"");
    if (i == std::string::npos)
        throw std::runtime_error("Unable to find date in lwg-issues.xml");
    i += sizeof("date=\"") - 1;
    auto j = s.find('\"', i);
    if (j == std::string::npos)
        throw std::runtime_error("Unable to parse date in lwg-issues.xml");
    return s.substr(i, j-i);
#endif
}


void format(std::vector<issue> & issues, issue & is) {
   std::string & s = is.text;
   int issue_num = is.num;
   std::vector<std::string> tag_stack;
   std::ostringstream er;
   // cannot rewrite as range-based for-loop as the string 's' is modified within the loop
   for (unsigned i = 0; i < s.size(); ++i) {
      if (s[i] == '<') {
         auto j = s.find('>', i);
         if (j == std::string::npos) {
            er.clear();
            er.str("");
            er << "missing '>' in issue " << issue_num;
            throw std::runtime_error{er.str()};
         }

         std::string tag;
         {
            std::istringstream iword{s.substr(i+1, j-i-1)};
            iword >> tag;
         }

         if (tag.empty()) {
             er.clear();
             er.str("");
             er << "unexpected <> in issue " << issue_num;
             throw std::runtime_error{er.str()};
         }

         if (tag[0] == '/') { // closing tag
             tag.erase(tag.begin());
             if (tag == "issue" || tag == "revision") {
                s.erase(i, j-i + 1);
                --i;
                return;
             }

             if (tag_stack.empty()  or  tag != tag_stack.back()) {
                er.clear();
                er.str("");
                er << "mismatched tags in issue " << issue_num;
                if (tag_stack.empty()) {
                   er << ".  Had no open tag.";
                }
                else {
                   er << ".  Open tag was " << tag_stack.back() << ".";
                }
                er << "  Closing tag was " << tag;
                throw std::runtime_error{er.str()};
             }

             tag_stack.pop_back();
             if (tag == "discussion") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "resolution") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "rationale") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "duplicate") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "note") {
                 s.replace(i, j-i + 1, "]</i></p>\n");
                 i += 9;
             }
             else {
                 i = j;
             }

             continue;
         }

         if (s[j-1] == '/') { // self-contained tag: sref, iref
            if (tag == "sref") {
               static const
               auto report_missing_quote = [](std::ostringstream & er, unsigned num) {
                  er.clear();
                  er.str("");
                  er << "missing '\"' in sref in issue " << num;
                  throw std::runtime_error{er.str()};
               };

               std::string r;
               auto k = s.find('\"', i+5);
               if (k >= j) {
                  report_missing_quote(er, issue_num);
               }

               auto l = s.find('\"', k+1);
               if (l >= j) {
                  report_missing_quote(er, issue_num);
               }

               ++k;
               r = s.substr(k, l-k);
         //                     if (section_db.count(r) == 0)
         //                     {
         //                         er.clear();
         //                         er.str("");
         //                         er << "unknown section reference: " << r << " in issue " << issue_num;
         //                         std::cout << er.str() << '\n';
         //                     }
               {
                  std::ostringstream t;
                  t << section_db[r] << ' ';
                  r.insert(0, t.str());
               }

               j -= i - 1;
               s.replace(i, j, r);
               i += r.size() - 1;
               continue;
            }
            else if (tag == "iref") {
               auto k = s.find('\"', i+5);
               if (k >= j) {
                     er.clear();
                     er.str("");
                     er << "missing '\"' in iref in issue " << issue_num;
                     throw std::runtime_error{er.str()};
               }
               auto l = s.find('\"', k+1);
               if (l >= j) {
                     er.clear();
                     er.str("");
                     er << "missing '\"' in iref in issue " << issue_num;
                     throw std::runtime_error{er.str()};
               }

               ++k;
               std::string r{s.substr(k, l-k)};
               std::istringstream temp{r};
               int num;
               temp >> num;
               if (temp.fail()) {
                  er.clear();
                  er.str("");
                  er << "bad number in iref in issue " << issue_num;
                  throw std::runtime_error{er.str()};
               }

               auto n = std::lower_bound(issues.begin(), issues.end(), num, sort_by_num{});
               if (n->num != num) {
                  er.clear();
                  er.str("");
                  er << "couldn't find number in iref in issue " << issue_num;
                  throw std::runtime_error{er.str()};
               }

               if (!tag_stack.empty()  and  tag_stack.back() == "duplicate") {
                  std::ostringstream temp;
                  temp << is.num;
                  std::string d = "<a href=\"";
                  d += find_file(is.stat);
                  d += '#';
                  d += temp.str();
                  d += "\">";
                  d += temp.str();
                  d += "</a>";
                  n->duplicates.push_back(d); // This is only reason vector is not const
                  temp.str("");
                  temp << n->num;
                  d = "<a href=\"";
                  d += find_file(n->stat);
                  d += '#';
                  d += temp.str();
                  d += "\">";
                  d += temp.str();
                  d += "</a>";
                  is.duplicates.push_back(d);
                  r.clear();
               }
               else {
                  static const
                  auto itos = [](int i) -> std::string {
                     std::ostringstream t;
                     t << i;
                     return t.str();
                  };
                  std::string ns = itos(num);
                  r = "<a href=\"";
                  r += find_file(n->stat);
                  r += '#';
                  r += ns;
                  r += "\">";
                  r += ns;
                  r += "</a>";
               }

               j -= i - 1;
               s.replace(i, j, r);
               i += r.size() - 1;
               continue;
            }
            else if (tag == "cd_N2800") {
               assert(!"not implemented yet");
               std::string r;
               auto k = s.find('\"', i+8);
               if (k >= j) {
                  er.clear();
                  er.str("");
                  er << "missing '\"' in cd_N2800 in issue " << issue_num;
                  throw std::runtime_error(er.str());
               }
               auto l = s.find('\"', k+1);
               if (l >= j) {
                  er.clear();
                  er.str("");
                  er << "missing '\"' in cd_N2800 in issue " << issue_num;
                  throw std::runtime_error(er.str());
               }
               ++k;
               r = s.substr(k, l-k);
               std::istringstream temp(r);
               std::string country;
               int cd_issue_num;
               temp >> country >> cd_issue_num;

               j -= i - 1;
               s.replace(i, j, r);
               i += r.size() - 1;
               continue;
            }
            i = j;
            continue;  // don't worry about this <tag/>
         }

         tag_stack.push_back(tag);
         if (tag == "discussion") {
             s.replace(i, j-i + 1, "<p><b>Discussion:</b></p>");
             i += 24;
         }
         else if (tag == "resolution") {
             s.replace(i, j-i + 1, "<p><b>Proposed resolution:</b></p>");
             i += 33;
         }
         else if (tag == "rationale") {
             s.replace(i, j-i + 1, "<p><b>Rationale:</b></p>");
             i += 23;
         }
         else if (tag == "duplicate") {
             s.erase(i, j-i + 1);
             --i;
         }
         else if (tag == "note") {
             s.replace(i, j-i + 1, "<p><i>[");
             i += 6;
         }
         else if (tag == "!--") {
             tag_stack.pop_back();
             j = s.find("-->", i);
             j += 3;
             s.erase(i, j-i);
             --i;
         }
         else {
             i = j;
         }
      }
   }
}


struct LwgIssuesXml {
   explicit LwgIssuesXml(std::string const & path);

   auto get_doc_number(std::string doc) const -> std::string;
   auto get_intro(std::string doc) const -> std::string;
   auto get_maintainer() const -> std::string;
   auto get_revision() const -> std::string;
   auto get_revisions(std::vector<issue> & issues) const -> std::string;
   auto get_statuses() const -> std::string;

private:
   std::string m_data;
};

LwgIssuesXml::LwgIssuesXml(std::string const & path)
   : m_data{}
   {
   std::ifstream infile{(path + "lwg-issues.xml").c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Unable to open lwg-issues.xml"};
   }

   std::istreambuf_iterator<char> first{infile}, last{};
   m_data.assign(first, last);
}


auto LwgIssuesXml::get_doc_number(std::string doc) const -> std::string {
    if (doc == "active") {
        doc = "active_docno=\"";
    }
    else if (doc == "defect") {
        doc = "defect_docno=\"";
    }
    else if (doc == "closed") {
        doc = "closed_docno=\"";
    }
    else {
        throw std::runtime_error{"unknown argument to get_doc_number: " + doc};
    }

    auto i = m_data.find(doc);
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find docno in lwg-issues.xml"};
    }
    i += doc.size();
    auto j = m_data.find('\"', i+1);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse docno in lwg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}

auto LwgIssuesXml::get_intro(std::string doc) const -> std::string {
    if (doc == "active") {
        doc = "<intro list=\"Active\">";
    }
    else if (doc == "defect") {
        doc = "<intro list=\"Defects\">";
    }
    else if (doc == "closed") {
        doc = "<intro list=\"Closed\">";
    }
    else {
        throw std::runtime_error{"unknown argument to intro: " + doc};
    }

    auto i = m_data.find(doc);
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find intro in lwg-issues.xml"};
    }
    i += doc.size();
    auto j = m_data.find("</intro>", i);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse intro in lwg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}


auto LwgIssuesXml::get_maintainer() const -> std::string {
   auto i = m_data.find("maintainer=\"");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find maintainer in lwg-issues.xml"};
   }
   i += sizeof("maintainer=\"") - 1;
   auto j = m_data.find('\"', i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer in lwg-issues.xml"};
   }
   std::string r = m_data.substr(i, j-i);
   auto m = r.find("&lt;");
   if (m == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer email address in lwg-issues.xml"};
   }
   m += sizeof("&lt;") - 1;
   auto me = r.find("&gt;", m);
   if (me == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer email address in lwg-issues.xml"};
   }
   std::string email = r.substr(m, me-m);
   // &lt;                                    lwgchair@gmail.com    &gt;
   // &lt;<a href="mailto:lwgchair@gmail.com">lwgchair@gmail.com</a>&gt;
   r.replace(m, me-m, "<a href=\"mailto:" + email + "\">" + email + "</a>");
   return r;
}


auto LwgIssuesXml::get_revision() const -> std::string {
    auto i = m_data.find("revision=\"");
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find revision in lwg-issues.xml"};
    }
    i += sizeof("revision=\"") - 1;
    auto j = m_data.find('\"', i);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse revision in lwg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}


auto LwgIssuesXml::get_revisions(std::vector<issue> & issues) const -> std::string {
   auto i = m_data.find("<revision_history>");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find <revision_history> in lwg-issues.xml"};
   }
   i += sizeof("<revision_history>") - 1;

   auto j = m_data.find("</revision_history>", i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to find </revision_history> in lwg-issues.xml"};
   }
   auto s = m_data.substr(i, j-i);
   j = 0;

   // bulding a potentially large string, would a stringstream be a better solution?
   // Probably not - string will not be *that* big, and stringstream pays the cost of locales
   std::string r = "<ul>\n";
   while (true) {
      i = s.find("<revision tag=\"", j);
      if (i == std::string::npos) {
         break;
      }
      i += sizeof("<revision tag=\"") - 1;
      j = s.find('\"', i);
      std::string const rv = s.substr(i, j-i);
      i = j+2;
      j = s.find("</revision>", i);
      issue is;
      is.text = s.substr(i, j-i);
      format(issues, is);
      r += "<li>";
      r += rv + ": ";
      r += is.text;
      r += "</li>\n";
   }
   r += "</ul>\n";
   return r;
}


auto LwgIssuesXml::get_statuses() const -> std::string {
   auto i = m_data.find("<statuses>");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find statuses in lwg-issues.xml"};
   }
   i += sizeof("<statuses>") - 1;

   auto j = m_data.find("</statuses>", i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to parse statuses in lwg-issues.xml"};
   }
   return m_data.substr(i, j-i);
}


void print_table(std::ostream& out, std::vector<issue>::const_iterator i, std::vector<issue>::const_iterator e) {
#if defined (DEBUG_LOGGING)
   std::cout << "\t" << std::distance(i,e) << " items to add to table" << std::endl;
#endif

   std::string prev_tag;
   for (; i != e; ++i) {
      out << "<tr>\n";

      // Number
      out << "<td align=\"right\">";
      out << "<a href=\"";
      out << find_file(i->stat);
      out << '#';
      out << i->num;
      out << "\">";
      out << i->num;
      out << "</a>";
      out << "</td>\n";

      // Status
      out << "<td align=\"left\"><a href=\"lwg-active.html#";
      out << remove_qualifier(i->stat);
      out << "\">";
      out << i->stat;
      out << "</a>";
      out << "<a name=\"" << i->num << "\"></a>";
      out << "</td>\n";

      // Section
      out << "<td align=\"left\">";
assert(!i->tags.empty());
      out << section_db[i->tags[0]] << " " << i->tags[0];
      if (i->tags[0] != prev_tag) {
         prev_tag = i->tags[0];
         out << "<a name=\"" << remove_square_brackets(prev_tag) << "\"</a>";
      }
      out << "</td>\n";

      // Title
      out << "<td align=\"left\">";
      out << i->title;
      out << "</td>\n";

      // Has Proposed Resolution
      out << "<td align=\"center\">";
      if (i->has_resolution) {
         out << "Yes";
      }
      else {
         out << "<font color=\"red\">No</font>";
      }
      out << "</td>\n";

      // Duplicates
      out << "<td align=\"left\">";
      if (!i->duplicates.empty()) {
         out << i->duplicates[0];
         for (unsigned j = 1; j < i->duplicates.size(); ++j) {
            out << ", " << i->duplicates[j];
         }
      }
      out << "</td>\n";

      // Modification date
      out << "<td align=\"center\">";
      out << i->mod_date.year() << '-';
      if (i->mod_date.month() < 10) { out << '0'; }
      out << i->mod_date.month() << '-';
      if (i->mod_date.day() < 10) { out << '0'; }
      out << i->mod_date.day() << "</p>\n";
      out << "</td>\n";

      out << "</tr>\n";
   }
   out << "</table>\n";
}


void print_file_header(std::ostream& out, std::string const & title) {
    out << "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\"\n";
    out << "    \"http://www.w3.org/TR/html4/strict.dtd\">\n";
    out << "<html>\n";
    out << "<head>\n";
    out << "<title>" << title << "</title>\n";
    out << "<style type=\"text/css\">\n";
    out << "p {text-align:justify}\n";
    out << "li {text-align:justify}\n";
    out << "blockquote.note\n";
    out << "{\n";
    out << "    background-color:#E0E0E0;\n";
    out << "    padding-left: 15px;\n";
    out << "    padding-right: 15px;\n";
    out << "    padding-top: 1px;\n";
    out << "    padding-bottom: 1px;\n";
    out << "}\n";
    out << insert_color;
    out << delete_color;
    out << "</style>\n";
    out << "</head>\n";
    out << "<body>\n";
}


void print_file_trailer(std::ostream& out) {
    out << "</body>\n";
    out << "</html>\n";
}


void make_sort_by_num(std::vector<issue>& issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
    sort(issues.begin(), issues.end(), sort_by_num{});

    std::ofstream out{(path + "lwg-toc.html").c_str()};
    out << "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n";
    out << "    \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n";
    out << "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n";
    out << "<head>\n";
    out << "<title>Library Issues List Table of Contents</title>\n";
    out << "<style type=\"text/css\">\n";
    out << "p {text-align:justify}\n";
    out << "li {text-align:justify}\n";
    out << "blockquote.note\n";
    out << "{\n";
    out << "    background-color:#E0E0E0;\n";
    out << "    padding-left: 15px;\n";
    out << "    padding-right: 15px;\n";
    out << "    padding-top: 1px;\n";
    out << "    padding-bottom: 1px;\n";
    out << "}\n";
    out << insert_color;
    out << delete_color;
    out << "</style>\n";
    out << "</head>\n";
    out << "<body>\n";

    out << "<h1>C++ Standard Library Issues List (Revision " << lwg_issues_xml.get_revision() << ")</h1>\n";
    out << "<h1>Table of Contents</h1>\n";
    out << "<p>Reference ISO/IEC IS 14882:2003(E)</p>\n";
    out << "<p>This document is the Table of Contents for the <a href=\"lwg-active.html\">Library Active Issues List</a>,"
           " <a href=\"lwg-defects.html\">Library Defect Reports List</a>, and <a href=\"lwg-closed.html\">Library Closed Issues List</a>.</p>\n";
    out << "<h2>Table of Contents</h2>\n";

    out << "<table border=\"1\" cellpadding=\"4\">\n";
    out << "<tr>\n";
    out << "<td align=\"center\"><b>Issue</b></td>\n";
    out << "<td align=\"center\"><a href=\"lwg-status.html\"><b>Status</b></a></td>\n";
    out << "<td align=\"center\"><a href=\"lwg-index.html\"><b>Section</b></a></td>\n";
    out << "<td align=\"center\"><b>Title</b></td>\n";
    out << "<td align=\"center\"><b>Proposed Resolution</b></td>\n";
    out << "<td align=\"center\"><b>Duplicates</b></td>\n";
    out << "<td align=\"center\"><a href=\"lwg-status-date.html\"><b>Last modified</b></a></td>\n";

    print_table(out, issues.begin(), issues.end());

    out << "</body>\n";
    out << "</html>\n";
}


void make_sort_by_status(std::vector<issue>& issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
    sort(issues.begin(), issues.end(), sort_by_num{});
    stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
    stable_sort(issues.begin(), issues.end(), sort_by_section{});
    stable_sort(issues.begin(), issues.end(), sort_by_status{});

    std::ofstream out{(path + "lwg-status.html").c_str()};
    out << "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n";
    out << "    \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n";
    out << "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n";
    out << "<head>\n";
    out << "<title>Library Issues List Table of Contents</title>\n";
    out << "<style type=\"text/css\">\n";
    out << "p {text-align:justify}\n";
    out << "li {text-align:justify}\n";
    out << "blockquote.note\n";
    out << "{\n";
    out << "    background-color:#E0E0E0;\n";
    out << "    padding-left: 15px;\n";
    out << "    padding-right: 15px;\n";
    out << "    padding-top: 1px;\n";
    out << "    padding-bottom: 1px;\n";
    out << "}\n";
    out << insert_color;
    out << delete_color;
    out << "</style>\n";
    out << "</head>\n";
    out << "<body>\n";

    out << "<h1>C++ Standard Library Issues List (Revision " << lwg_issues_xml.get_revision() << ")</h1>\n";
    out << "<h1>Table of Contents</h1>\n";
    out << "<p>Reference ISO/IEC IS 14882:2003(E)</p>\n";
    out << "<p>This document is the Table of Contents for the <a href=\"lwg-active.html\">Library Active Issues List</a>,"
           " <a href=\"lwg-defects.html\">Library Defect Reports List</a>, and <a href=\"lwg-closed.html\">Library Closed Issues List</a>.</p>\n";

    out << "<h2>Index by Status</h2>\n";

    for (auto i = issues.cbegin(), e = issues.cend(); i != e;) {
        auto const & current_status = i->stat;
        // should be thinking in terms of adjactent_find
        auto j = std::find_if(i, e, [&](issue const & iss){ return iss.stat != current_status; } );
        out << "<h2><a name=\"" << current_status << "\"</a>" << current_status << " (" << (j-i) << " issues)</h2>\n";

        out << "<table border=\"1\" cellpadding=\"4\">\n";
        out << "<tr>\n";
        out << "<td align=\"center\"><a href=\"lwg-toc.html\"><b>Issue</b></a></td>\n";
        out << "<td align=\"center\"><b>Status</b></td>\n";
        out << "<td align=\"center\"><a href=\"lwg-index.html\"><b>Section</b></a></td>\n";
        out << "<td align=\"center\"><b>Title</b></td>\n";
        out << "<td align=\"center\"><b>Proposed Resolution</b></td>\n";
        out << "<td align=\"center\"><b>Duplicates</b></td>\n";
        out << "<td align=\"center\"><a href=\"lwg-status-date.html\"><b>Last modified</b></a></td>\n";

        print_table(out, i, j);
        i = j;
    }

    out << "</body>\n";
    out << "</html>\n";
}

void make_sort_by_status_mod_date(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
    sort(issues.begin(), issues.end(), sort_by_num{});
    stable_sort(issues.begin(), issues.end(), sort_by_section{});
    stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
    stable_sort(issues.begin(), issues.end(), sort_by_status{});

    std::ofstream out{(path + "lwg-status-date.html").c_str()};
    out << "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n";
    out << "    \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n";
    out << "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n";
    out << "<head>\n";
    out << "<title>Library Issues List Table of Contents</title>\n";
    out << "<style type=\"text/css\">\n";
    out << "p {text-align:justify}\n";
    out << "li {text-align:justify}\n";
    out << "blockquote.note\n";
    out << "{\n";
    out << "    background-color:#E0E0E0;\n";
    out << "    padding-left: 15px;\n";
    out << "    padding-right: 15px;\n";
    out << "    padding-top: 1px;\n";
    out << "    padding-bottom: 1px;\n";
    out << "}\n";
    out << insert_color;
    out << delete_color;
    out << "</style>\n";
    out << "</head>\n";
    out << "<body>\n";

    out << "<h1>C++ Standard Library Issues List (Revision " << lwg_issues_xml.get_revision() << ")</h1>\n";
    out << "<h1>Table of Contents</h1>\n";
    out << "<p>Reference ISO/IEC IS 14882:2003(E)</p>\n";
    out << "<p>This document is the Table of Contents for the <a href=\"lwg-active.html\">Library Active Issues List</a>,"
           " <a href=\"lwg-defects.html\">Library Defect Reports List</a>, and <a href=\"lwg-closed.html\">Library Closed Issues List</a>.</p>\n";

    out << "<h2>Index by Status</h2>\n";

    for (auto i = issues.cbegin(), e = issues.cend(); i != e;) {
        std::string const & current_status = i->stat;
        auto j = find_if(i, e, [current_status](issue const & iss){ return iss.stat != current_status; } );
        out << "<h2><a name=\"" << current_status << "\"</a>" << current_status << " (" << (j-i) << " issues)</h2>\n";

        out << "<table border=\"1\" cellpadding=\"4\">\n";
        out << "<tr>\n";
        out << "<td align=\"center\"><a href=\"lwg-toc.html\"><b>Issue</b></a></td>\n";
        out << "<td align=\"center\"><a href=\"lwg-status.html\"><b>Status</b></a></td>\n";
        out << "<td align=\"center\"><a href=\"lwg-index.html\"><b>Section</b></a></td>\n";
        out << "<td align=\"center\"><b>Title</b></td>\n";
        out << "<td align=\"center\"><b>Proposed Resolution</b></td>\n";
        out << "<td align=\"center\"><b>Duplicates</b></td>\n";
        out << "<td align=\"center\"><b>Last modified</b></td></tr>\n";

        print_table(out, i, j);
        i = j;
    }

    out << "</body>\n";
    out << "</html>\n";
}

auto major_section(section_num const & sn) -> std::string {
   std::string const prefix{sn.prefix};
   std::ostringstream out;
   if (!prefix.empty()) {
      out << prefix << " ";
   }
   if (sn.num[0] < 100) {
      out << sn.num[0];
   }
   else {
      out << char{sn.num[0] - 100 + 'A'};
   }
   return out.str();
}

struct sort_by_mjr_section {
    auto operator()(const issue& x, const issue& y) const -> bool {
assert(!x.tags.empty());
assert(!y.tags.empty());
        const section_num& xn = section_db[x.tags[0]];
        const section_num& yn = section_db[y.tags[0]];
        return xn.prefix < yn.prefix ? true
             : xn.prefix > yn.prefix ? false
             : xn.num[0] < yn.num[0];  //  default
    }
};

void make_sort_by_section(std::vector<issue>& issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml, bool active_only = false) {
   sort(issues.begin(), issues.end(), sort_by_num{});
   stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
   stable_sort(issues.begin(), issues.end(), sort_by_status{});
   auto b = issues.begin();
   auto e = issues.end();
   if(active_only) {
      b = find_if(b, e, [](issue const & iss){ return "Ready" != iss.stat; });
      e = find_if(b, e, [](issue const & iss){ return !is_active(iss.stat); });
   }
   stable_sort(b, e, sort_by_section{});
   std::set<issue, sort_by_mjr_section> mjr_section_open;
   for (auto const & elem : issues ) {
      if (is_active_not_ready(elem.stat)) {
         mjr_section_open.insert(elem);
      }
   }

   std::string filename = active_only
                        ? path + "lwg-index-open.html"
                        : path + "lwg-index.html";
   std::ofstream out(filename.c_str());
   out << "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n";
   out << "    \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n";
   out << "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n";
   out << "<head>\n";
   out << "<title>Library Issues List Table of Contents</title>\n";
   out << "<style type=\"text/css\">\n";
   out << "p {text-align:justify}\n";
   out << "li {text-align:justify}\n";
   out << "blockquote.note\n";
   out << "{\n";
   out << "    background-color:#E0E0E0;\n";
   out << "    padding-left: 15px;\n";
   out << "    padding-right: 15px;\n";
   out << "    padding-top: 1px;\n";
   out << "    padding-bottom: 1px;\n";
   out << "}\n";
   out << insert_color;
   out << delete_color;
   out << "</style>\n";
   out << "</head>\n";
   out << "<body>\n";

   out << "<h1>C++ Standard Library Issues List (Revision " << lwg_issues_xml.get_revision() << ")</h1>\n";
   out << "<h1>Table of Contents</h1>\n";
   out << "<p>Reference ISO/IEC IS 14882:2003(E)</p>\n";
   out << "<p>This document is the Table of Contents for the <a href=\"lwg-active.html\">Library Active Issues List</a>,"
          " <a href=\"lwg-defects.html\">Library Defect Reports List</a>, and <a href=\"lwg-closed.html\">Library Closed Issues List</a>.</p>\n";
   out << "<h2>Index by Section";
   if (active_only) {
      out << " (non-Ready active issues only)";
   }
   out << "</h2>\n";
   if (active_only) {
      out << "<p><a href=\"lwg-index.html\">(view all issues)</a></p>\n";
   }
   else {
      out << "<p><a href=\"lwg-index-open.html\">(view only non-Ready open issues)</a></p>\n";
   }

   // Would prefer to use const_iterators from here, but oh well....
   for (auto i = b; i != e;) {
assert(!i->tags.empty());
      int current_num = section_db[i->tags[0]].num[0];
      auto j = i;
      for (; j != e; ++j) {
         if (section_db[j->tags[0]].num[0] != current_num) {
             break;
         }
      }
      std::string const msn{major_section(section_db[i->tags[0]])};
      out << "<h2><a name=\"Section " << msn << "\"></a>" << "Section " << msn;
      out << " (" << (j-i) << " issues)</h2>\n";
      out << "<p>";
      if (active_only) {
         out << "<a href=\"lwg-index.html#Section " << msn << "\">(view all issues)</a>";
      }
      else if (mjr_section_open.count(*i) > 0) {
         out << "<a href=\"lwg-index-open.html#Section " << msn << "\">(view only non-Ready open issues)</a>";
      }
      out << "</p>\n";

      out << "<table border=\"1\" cellpadding=\"4\">\n";
      out << "<tr>\n";
      out << "<td align=\"center\"><a href=\"lwg-toc.html\"><b>Issue</b></a></td>\n";
      out << "<td align=\"center\"><a href=\"lwg-status.html\"><b>Status</b></a></td>\n";
      out << "<td align=\"center\"><b>Section</b></td>\n";
      out << "<td align=\"center\"><b>Title</b></td>\n";
      out << "<td align=\"center\"><b>Proposed Resolution</b></td>\n";
      out << "<td align=\"center\"><b>Duplicates</b></td>\n";
      out << "<td align=\"center\"><a href=\"lwg-status-date.html\"><b>Last modified</b></a></td>\n";

      print_table(out, i, j);
      i = j;
   }

   out << "</body>\n";
   out << "</html>\n";
}


template <class Pred>
void print_issues(std::ostream& out, const std::vector<issue>& issues, Pred pred) {
   std::multiset<issue, sort_by_first_tag> const  all_issues{ issues.begin(), issues.end()} ;
   std::multiset<issue, sort_by_status>    const  issues_by_status{ issues.begin(), issues.end() };

   std::multiset<issue, sort_by_first_tag> active_issues;
   for (auto const & elem : issues ) {
      if (is_active(elem.stat)) {
         active_issues.insert(elem);
      }
   }

   for (auto const & iss : issues) {
      if (pred(iss)) {
         out << "<hr>\n";

         // Number and title
         out << "<h3>";
         out << "<a name=\"";
         out << iss.num;
         out << "\"></a>";
         out << iss.num << ". " << iss.title << "</h3>\n";

         // Section, Status, Submitter, Date
         out << "<p><b>Section:</b> ";
         out << section_db[iss.tags[0]] << " " << iss.tags[0];
         for (unsigned k = 1; k < iss.tags.size(); ++k) {
            out << ", " << section_db[iss.tags[k]] << " " << iss.tags[k];
         }

         out << " <b>Status:</b> <a href=\"lwg-active.html#" << remove_qualifier(iss.stat) << "\">" << iss.stat << "</a>\n";
         out << " <b>Submitter:</b> " << iss.submitter;
         out << " <b>Opened:</b> " << iss.date.year() << '-';
         if (iss.date.month() < 10)       { out << '0'; }
         out << iss.date.month() << '-';
         if (iss.date.day() < 10)         { out << '0'; }
         out << iss.date.day() << " ";
         out << " <b>Last modified:</b> " << iss.mod_date.year() << '-';
         if (iss.mod_date.month() < 10)   { out << '0'; }
         out << iss.mod_date.month() << '-';
         if (iss.mod_date.day() < 10)     { out << '0'; }
         out << iss.mod_date.day() << "</p>\n";

         // view active issues in []
         if (active_issues.count(iss) > 1) {
            out << "<p><b>View other</b> " << "<a href=\"" << "lwg-index-open.html#" << remove_square_brackets(iss.tags[0])
                << "\">" << "active issues</a> in " << iss.tags[0] << ".</p>\n";
         }

         // view all issues in []
         if (all_issues.count(iss) > 1) {
            out << "<p><b>View all other</b> " << "<a href=\"" << "lwg-index.html#" << remove_square_brackets(iss.tags[0])
                << "\">" << "issues</a> in " << iss.tags[0] << ".</p>\n";
         }
         // view all issues with same status
         if (issues_by_status.count(iss) > 1) {
            out << "<p><b>View all issues with</b> "
                << "<a href=\"lwg-status.html#" << iss.stat << "\">" << iss.stat << "</a>"
                << " status.</p>\n";
         }

         // duplicates
         if (!iss.duplicates.empty()) {
            out << "<p><b>Duplicate of:</b> ";
            out << iss.duplicates[0];
            for (unsigned j = 1; j < iss.duplicates.size(); ++j)
            out << ", " << iss.duplicates[j];
            out << "</p>\n";
         }

         // text
         out << iss.text << "\n\n";
      }
   }
}

void print_paper_heading(std::ostream& out, std::string const & paper, LwgIssuesXml const & lwg_issues_xml) {
   out << "<table>\n";
   out << "<tr>\n";
   out << "<td align=\"left\">Doc. no.</td>\n";
   out << "<td align=\"left\">" << lwg_issues_xml.get_doc_number(paper) << "</td>\n";
   out << "</tr>\n";
   out << "<tr>\n";
   out << "<td align=\"left\">Date:</td>\n";
   out << "<td align=\"left\">" << get_date() << "</td>\n";
   out << "</tr>\n";
   out << "<tr>\n";
   out << "<td align=\"left\">Project:</td>\n";
   out << "<td align=\"left\">Programming Language C++</td>\n";
   out << "</tr>\n";
   out << "<tr>\n";
   out << "<td align=\"left\">Reply to:</td>\n";
   out << "<td align=\"left\">" << lwg_issues_xml.get_maintainer() << "</td>\n";
   out << "</tr>\n";
   out << "</table>\n";
   out << "<h1>";
   if (paper == "active") {
      out << "C++ Standard Library Active Issues List (Revision ";
   }
   else if (paper == "defect") {
      out << "C++ Standard Library Defect Report List (Revision ";
   }
   else if (paper == "closed") {
      out << "C++ Standard Library Closed Issues List (Revision ";
   }
   out << lwg_issues_xml.get_revision() << ")</h1>\n";
}


// Functions to make the 3 standard published issues list documents
// A precondition for calling any of these functions is that the list of issues is sorted in numerical order, by issue number.
// While nothing disasterous will happen if this precondition is violated, the published issues list will list items
// in the wrong order.
void make_active(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));
   std::ofstream out{(path + "lwg-active.html").c_str()};
   print_file_header(out, "C++ Standard Library Active Issues List");
   print_paper_heading(out, "active", lwg_issues_xml);
   out << lwg_issues_xml.get_intro("active") << '\n';
   out << "<h2>Revision History</h2>\n" << lwg_issues_xml.get_revisions(issues) << '\n';
   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << lwg_issues_xml.get_statuses() << '\n';
   out << "<h2>Active Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_active(i.stat);} );
   print_file_trailer(out);
}


void make_defect(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));
   std::ofstream out((path + "lwg-defects.html").c_str());
   print_file_header(out, "C++ Standard Library Defect Report List");
   print_paper_heading(out, "defect", lwg_issues_xml);
   out << lwg_issues_xml.get_intro("defect") << '\n';
   out << "<h2>Revision History</h2>\n" << lwg_issues_xml.get_revisions(issues) << '\n';
   out << "<h2>Defect Reports</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_defect(i.stat);} );
   print_file_trailer(out);
}


void make_closed(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));
   std::ofstream out{(path + "lwg-closed.html").c_str()};
   print_file_header(out, "C++ Standard Library Closed Issues List");
   print_paper_heading(out, "closed", lwg_issues_xml);
   out << lwg_issues_xml.get_intro("closed") << '\n';
   out << "<h2>Revision History</h2>\n" << lwg_issues_xml.get_revisions(issues) << '\n';
   out << "<h2>Closed Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_closed(i.stat);} );
   print_file_trailer(out);
}


// Additional non-standard documents, useful for running LWG meetings
void make_tentative(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
   // publish a document listing all tentative issues that may be acted on during a meeting.
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));
   std::ofstream out{(path + "lwg-tentative.html").c_str()};
   print_file_header(out, "C++ Standard Library Tentative Issues");
//   print_paper_heading(out, "active", lwg_issues_xml);
//   out << lwg_issues_xml.get_intro("active") << '\n';
//   out << "<h2>Revision History</h2>\n" << lwg_issues_xml.get_revisions(issues) << '\n';
//   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << lwg_issues_xml.get_statuses() << '\n';
   out << "<h2>Tentative Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_tentative(i.stat);} );
   print_file_trailer(out);
}

void make_unresolved(std::vector<issue> & issues, std::string const & path, LwgIssuesXml const & lwg_issues_xml) {
   // publish a document listing all non-tentative, non-ready issues that must be reviewed during a meeting.
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));
   std::ofstream out{(path + "lwg-unresolved.html").c_str()};
   print_file_header(out, "C++ Standard Library Unresolved Issues");
//   print_paper_heading(out, "active", lwg_issues_xml);
//   out << lwg_issues_xml.get_intro("active") << '\n';
//   out << "<h2>Revision History</h2>\n" << lwg_issues_xml.get_revisions(issues) << '\n';
//   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << lwg_issues_xml.get_statuses() << '\n';
   out << "<h2>Unresolved Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_not_resolved(i.stat);} );
   print_file_trailer(out);
}


auto parse_month(std::string const & m) -> int {
   // This could be turned into an efficient map lookup with a suitable indexed container
   return (m == "Jan") ?  1
        : (m == "Feb") ?  2
        : (m == "Mar") ?  3
        : (m == "Apr") ?  4
        : (m == "May") ?  5
        : (m == "Jun") ?  6
        : (m == "Jul") ?  7
        : (m == "Aug") ?  8
        : (m == "Sep") ?  9
        : (m == "Oct") ? 10
        : (m == "Nov") ? 11
        : (m == "Dec") ? 12
        : throw std::runtime_error{"unknown month"};
}


auto parse_issue_from_file(std::string const & filename) -> issue {
   using std::runtime_error;

   auto tx = read_file_into_string(filename);

   issue is;
        // Get issue number
        auto k = tx.find("<issue num=\"");
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue number in " + filename};
        }
        k += sizeof("<issue num=\"") - 1;
        auto l = tx.find('\"', k);
        std::istringstream temp{tx.substr(k, l-k)};
        temp >> is.num;

        // Get issue status
        k = tx.find("status=\"", l);
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue status in " + filename};
        }
        k += sizeof("status=\"") - 1;
        l = tx.find('\"', k);
        is.stat = tx.substr(k, l-k);

        // Get issue title
        k = tx.find("<title>", l);
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue title in " + filename};
        }
        k +=  sizeof("<title>") - 1;
        l = tx.find("</title>", k);
        is.title = tx.substr(k, l-k);

        // Get issue sections
        k = tx.find("<section>", l);
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue section in " + filename};
        }
        k += sizeof("<section>") - 1;
        l = tx.find("</section>", k);
        while (k < l) {
            k = tx.find('\"', k);
            if (k >= l) {
                break;
            }
            auto k2 = tx.find('\"', k+1);
            if (k2 >= l) {
               throw runtime_error{"Unable to find issue section in " + filename};
            }
            ++k;
            is.tags.push_back(tx.substr(k, k2-k));
            if (section_db.find(is.tags.back()) == section_db.end()) {
                section_num num{};
                num.num.push_back(100 + 'X' - 'A');
                section_db[is.tags.back()] = num;
            }
            k = k2;
            ++k;
        }

        if (is.tags.empty()) {
            throw runtime_error{"Unable to find issue section in " + filename};
        }

        // Get submitter
        k = tx.find("<submitter>", l);
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue submitter in " + filename};
        }
        k += sizeof("<submitter>") - 1;
        l = tx.find("</submitter>", k);
        is.submitter = tx.substr(k, l-k);

        // Get date
        k = tx.find("<date>", l);
        if (k == std::string::npos) {
            throw runtime_error{"Unable to find issue date in " + filename};
        }
        k += sizeof("<date>") - 1;
        l = tx.find("</date>", k);
        temp.clear();
        temp.str(tx.substr(k, l-k));
        {
           int d;
           temp >> d;
           if (temp.fail()) {
               throw std::runtime_error{"date format error in " + filename};
           }

           std::string month;
           temp >> month;

           try {
              int m{ parse_month(month) };
              int y{ 0 };
              temp >> y;
              is.date = greg::month{m}/greg::day{d}/y;
           }
           catch (std::exception const & e) {
              throw std::runtime_error{e.what() + std::string{" in "} + filename};
           }
        }

      // Get modification date
      {
         struct stat buf;
         if (stat(filename.c_str(), &buf) == -1) {
            throw std::runtime_error{"call to stat failed for " + filename};
         }

         struct tm* mod = std::localtime(&buf.st_mtime);
         is.mod_date = greg::year((unsigned short)(mod->tm_year+1900)) / (mod->tm_mon+1) / mod->tm_mday;
      }

      // Trim text to <discussion>
      k = tx.find("<discussion>", l);
      if (k == std::string::npos) {
         throw runtime_error{"Unable to find issue discussion in " + filename};
      }
      tx.erase(0, k);

      // Find out if issue has a proposed resolution
      if (is_active(is.stat)) {
         auto k2 = tx.find("<resolution>", 0);
         if (k2 == std::string::npos) {
            is.has_resolution = false;
         }
         else {
            k2 += sizeof("<resolution>") - 1;
            auto l2 = tx.find("</resolution>", k2);
            is.has_resolution = l2 - k2 > 15;
         }
      }
      else {
         is.has_resolution = true;
      }

      is.text = tx;
      return is;
}


auto read_issues(std::string const & issues_path) -> std::vector<issue> {
   std::cout << "Reading issues from: " << issues_path << std::endl;

   DIR* dir = opendir(issues_path.c_str());
   if (dir == 0) {
      throw std::runtime_error{"Unable to open issues dir"};
   }

   std::vector<issue> issues{};
   while ( dirent* entry = readdir(dir) ) {
      std::string const issue_file{ entry->d_name };
      if (issue_file.find("issue") != 0) {
         continue;
      }

      issues.push_back(parse_issue_from_file(issues_path + issue_file));
   }
   closedir(dir);
   return issues;
}


// ============================================================================================================

int main(int argc, char* argv[]) {
   try {
      std::string path;
      std::cout << "Preparing new LWG issues lists..." << std::endl;
      if (argc == 2) {
         path = argv[1];
      }
      else {
         char cwd[1024];
         if (getcwd(cwd, sizeof(cwd)) == 0) {
            std::cout << "unable to getcwd\n";
            return 1;
         }
         path = cwd;
      }

      if (path.back() != '/') { path += '/'; }
      section_db  = read_section_db(path + "meta-data/");

      auto issues_path = path + "xml/";
      LwgIssuesXml lwg_issues_xml(issues_path);


      //    check_against_index(section_db);
      auto issues = read_issues(issues_path);

      // Initially sort the issues by issue number, so each issue can be correctly 'format'ted
      sort(issues.begin(), issues.end(), sort_by_num{});
      for( auto & i : issues ) { format(issues, i); }

      // Now we have a parsed and formatted set of issues, we can write the standard set of HTML documents
      make_sort_by_num(issues, path, lwg_issues_xml);
      make_sort_by_status(issues, path, lwg_issues_xml);
      make_sort_by_status_mod_date(issues, path, lwg_issues_xml);
      make_sort_by_section(issues, path, lwg_issues_xml);
      make_sort_by_section(issues, path, lwg_issues_xml, true);

      // issues must be sorted by number before making the mailing list documents
      sort(issues.begin(), issues.end(), sort_by_num{});

      make_active(issues, path, lwg_issues_xml);
      make_defect(issues, path, lwg_issues_xml);
      make_closed(issues, path, lwg_issues_xml);

      // unofficial documents
      make_tentative(issues, path, lwg_issues_xml);
      make_unresolved(issues, path, lwg_issues_xml);

      std::cout << "Made all documents\n";
   }
   catch(std::exception const & ex) {
      std::cout << ex.what() << std::endl;
      return 1;
   }
}