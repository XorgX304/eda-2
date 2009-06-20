// InstructionFactoryISDF.cc -- June 18, 2009
//    by geohot
// part of "The Embedded Disassembler"
//  released under GPLv3, see http://gplv3.fsf.org/
//
// This parses ISDF files, and behaves like a normal instruction factory

#include "InstructionFactoryISDF.h"
#include <string>
#include <fstream>
#include <iostream>

#include "debug.h"
#include "file.h"

using namespace std;
using namespace eda;

// Register parser goes here, not in InstructionComprehensions
InstructionFactoryISDF::InstructionFactoryISDF(string filename, Memory* m) {
  File::ReadFileToString(kBaseDirectory+filename, &store_);
  int i = 0;
  current_line_ = 1;
  memory_ = m;
  InstructionComprehension* current = NULL;
  while(i < store_.length()) {
    int pos = store_.find_first_of('\n', i);
    string line = store_.substr(i, pos-i);
    line_vector_debug_.push_back(line);
    //cout << "XXXX" << line << "XXXX" << endl;
    i += (pos-i)+1;
    if(line.length() > 0 && line[0] != '#') {
      string first_word = line.substr(0, line.find_first_of(' ', 0));
      if(first_word == "Registers") {
        vector<string> registers;
        StringSplit(' ', line, &registers);
        for (int i = 1; i < registers.size(); i++) {
          registers_.push_back(make_pair(registers[i], m->AllocateSegment(registers[i], 4)));
        }
      } else if(first_word == "DefaultChange") {
        // 0 is Change, 1 is bits, 2 is target, 3... is change
        vector<string> change;
        StringSplit(' ', line, &change);
        string changesource = "";
        for (int i = 3; i < change.size(); i++) {
          changesource += change[i];    // remove spaces for the hell of it
        }
        default_changes_.insert(make_pair(change[2], make_pair(stoi(change[1]), changesource)));
      } else if(line[0] == '0' || line[0] == '1' || line[0] == '*' || (line[0] >= 'a' && line[0] <= 'z') ) {
        if(current != NULL) instructioncomprehensions_.push_back(current);
        current = new InstructionComprehension(line, this);
      } else if(line.length() > 2 && line[0] == ' ' && line[1] == ' ' && current != NULL) {
        current->AddLine(line);
      } else if(first_word.length() > 0) {
        global_scope_.insert(make_pair(first_word, line.substr(first_word.length()+1)));
      } else {
        LOG(INFO) << "Unparsed line(" << current_line_ << "): " << line;
      }
    }
    //LOG(INFO) << "parsed line " << current_line_;
    current_line_++;
  }
  if(current != NULL) instructioncomprehensions_.push_back(current);
  LOG(INFO) << "read " << current_line_ << " lines of comprehension";
}

Address* InstructionFactoryISDF::Process(Address* start) {
  uint32_t opcode;
  Address* ret = start->get32(0, &opcode);  // hmm, bad size

  map<string, string> global_scope_copy = global_scope_;

  StatelessChangelist* change = new StatelessChangelist;
  ParsedInstruction* parsed = new ParsedInstruction;

  for(vector<InstructionComprehension*>::iterator it = instructioncomprehensions_.begin(); it != instructioncomprehensions_.end(); ++it) {
    if((*it)->Execute(opcode, &global_scope_copy, change, parsed) == true) break;
  }

  for(map<string, pair<int, string> >::iterator it = default_changes_.begin(); it != default_changes_.end(); ++it) {
    string changetarget = it->first;
    string changesource = it->second.second;
    int size = it->second.first;

    StatelessChangelistIterator ic;
    for(ic = change->changes_.begin(); ic != change->changes_.end(); ++ic) {
      if(ic->first.first == changetarget) break;
    }
    if(ic == change->changes_.end()) {
      LOG(DEBUG) << "adding default change " << changetarget << " ("<< size << ")=(1) " << changesource;
      change->add_change(changetarget, "1", size/8, changesource);
    } else if(ic->first.second != "1") {
      LOG(DEBUG) << "adding default change against condition " << changetarget << " ("<< size << ")=(" << ic->first.second << ") " << changesource;
      change->add_change(changetarget, "~("+ic->first.second+")", size/8, changesource);
    }
  }

  DebugPrint(change);
  LOG(DEBUG) << "parsestr -- " << parsed->format_;
  LOG(DEBUG) << "Parsed -- " << parsed->GetConsoleString();

  start->set_instruction(new Instruction(parsed, change, start, 4));
  return ret;
}

// Okay, I know I'm not supposed to work in the constructor...
InstructionComprehension::InstructionComprehension(const string& firstline, InstructionFactoryISDF* ISDF) {
  // First line should be a data+mask
  data_ = 0;
  mask_ = 0;
  char lastchar = '\0';
  int bitfield_start;
  uint32_t local_mask = 0;
  bitsize_ = 0;
  parsed_type_ = 0;

  parent_ = ISDF;

  line_= ISDF->current_line_;

  stop_flag_ = false;

// Parse first line
  for (int i = 0; i < firstline.length(); i++) {
    char thischar = firstline[i];
    if (lastchar != '\0' && lastchar != thischar) {
      // Add a local variable
      local_scope_additions_.insert(make_pair(lastchar, make_pair(bitfield_start, local_mask)));
      lastchar = '\0';
      local_mask = 0;
    }
    if (thischar == '0') {
      data_ = (data_ << 1); mask_ = (mask_ << 1) | 1; bitsize_++;
    } else if (thischar == '1') {
      data_ = (data_ << 1) | 1; mask_ = (mask_ << 1) | 1; bitsize_++;
    } else if (thischar == '*') {
      data_ = (data_ << 1); mask_ = (mask_ << 1); bitsize_++;
    } else if (thischar >= 'a' && thischar <= 'z' ) {
      data_ = (data_ << 1); mask_ = (mask_ << 1); bitsize_++;
      local_mask = (local_mask<<1) | 1;
      bitfield_start = bitsize_;
      lastchar = thischar;
    } else if (thischar == ' ') {
      //ignore spaces
    } else {
      LOG(INFO) << "Invalid character on line " << parent_->current_line_ << " at " << bitsize_ << ": " << thischar;
    }
  }
  if (lastchar != '\0' ) {
    local_scope_additions_.insert(make_pair(lastchar, make_pair(bitfield_start, local_mask)));
  }
  if ( (bitsize_%8) != 0) {
    LOG(INFO) << "Instruction comprehension on line " << parent_->current_line_ << " isn't a multiple of 8 bits long";
  }
}

void InstructionComprehension::AddLine(const string& linein) {
  string line = linein.substr(linein.find_first_not_of(' ', 0));
  string first_word = line.substr(0, line.find_first_of(' ', 0));
  //LOG(INFO) << "Got word " << first_word << " at line " << parent_->current_line_;
  if(first_word ==  "Change") {
    // 0 is Change, 1 is bits, 2 is target, 3... is change
    vector<string> change;
    StringSplit(' ', line, &change);
    string changesource = "";
    for (int i = 3; i < change.size(); i++) {
      changesource += change[i];    // remove spaces for the hell of it
    }
    changes_.insert(make_pair(change[2], make_pair(stoi(change[1]), changesource)));
  } else if(first_word == "Parsed") {
    int startquote = line.find_first_of('"',0);
    int endquote = line.find_first_of('"', startquote+1);
    parsed_format_ = line.substr(startquote+1, endquote-startquote-1);
    //LOG(DEBUG) << "Read parsedata " << parsed_format_;
    if(line.length() > (endquote+2)) {
      StringSplit(' ', line.substr(endquote+2), &parsed_atoms_);
    }
    if(parsed_format_[0] == '%') {
      parsed_type_ = 2; //APPEND
      parsed_format_ = parsed_format_.substr(1);
    } else if(parsed_format_[parsed_format_.length()-1] == '%') {
      parsed_type_ = 1; //PREPEND
      parsed_format_ = parsed_format_.substr(0, parsed_format_.length()-1);
    } else parsed_type_ = 3;
  } else if(first_word == "Stop") {
    stop_flag_ = true;
  } else {
    // Default is a global_scope string
    global_scope_additions_.insert(make_pair(first_word, line.substr(first_word.length()+1)));
  }
}

bool InstructionComprehension::Execute(uint32_t data, map<string, string>* global_scope, StatelessChangelist* change, ParsedInstruction* parsed) {
  if( (data & mask_) != data_) {
    //LOG(DEBUG) << std::hex << "No match on data " << data << " with data " << data_ << " and mask " << mask_;
    return false;
  }

  LOG(INFO) << "Running comprehension at " << line_ << ": " << parent_->line_vector_debug_[line_-1] << " " << parent_->line_vector_debug_[line_-2];

  map<char, uint32_t> local_scope;
// Evaluate local scope
  for(map<char, pair<int, uint32_t> >::iterator it = local_scope_additions_.begin(); it != local_scope_additions_.end(); ++it) {
    local_scope.insert(make_pair(it->first, (data >> (bitsize_ - it->second.first)) & it->second.second ));
    LOG(DEBUG) << "added " << it->first << " to the local scope: " << local_scope[it->first] << " DBG " << bitsize_ << " " << it->second.first << " " << it->second.second;
  }
// Evaluate global scope
  for(map<string, string>::iterator it = global_scope_additions_.begin(); it != global_scope_additions_.end(); ++it) {
    (*global_scope)[it->first] = parent_->EvalulateStringInScope(*global_scope, local_scope, it->second);
    LOG(DEBUG) << "added " << it->first << " to the global scope: " << (*global_scope)[it->first];
  }
// Add Parsed
  if(parsed_type_ > 0) {
    LOG(DEBUG) << "Adding parsedata of type(" << parsed_type_ << "): " << parsed_format_;
    if(parsed_type_ == 3) { // clear
      parsed->format_ = "";
      parsed->args_.clear();
    }
    if(parsed_type_ == 1) { // prepend
      parsed->format_ = parent_->EvalulateStringInScope(*global_scope, local_scope, parsed_format_) + parsed->format_;
      vector<string>::iterator pos = parsed->args_.begin();
      for(vector<string>::iterator it = parsed_atoms_.begin(); it != parsed_atoms_.end(); ++it) {
        string eval = parent_->EvalulateStringInScope(*global_scope, local_scope, *it);
        pos = parsed->args_.insert(pos, eval);
        ++pos;
      }
    } else if(parsed_type_ >= 2) { //append or clear
      parsed->format_ += parent_->EvalulateStringInScope(*global_scope, local_scope, parsed_format_);
      for(vector<string>::iterator it = parsed_atoms_.begin(); it != parsed_atoms_.end(); ++it) {
        string eval = parent_->EvalulateStringInScope(*global_scope, local_scope, *it);
        parsed->args_.push_back(eval);
      }
    }
  }
// Add changes
  for(map<string, pair<int, string> >::iterator it = changes_.begin(); it != changes_.end(); ++it) {
    string changetarget = parent_->EvalulateStringInScope(*global_scope, local_scope, it->first);
    string changesource = parent_->EvalulateStringInScope(*global_scope, local_scope, it->second.second);
    string condition = (*global_scope)["Condition"];
    int size = it->second.first;
    LOG(DEBUG) << "adding change " << changetarget << " ("<< size << ")=(" << condition << ") " << changesource;
    change->add_change(changetarget, condition, size/8, changesource);
  }

  return stop_flag_;
}


// Called by InstructionComprehension.Execute
string InstructionFactoryISDF::EvalulateStringInScope(const map<string, string>& global_scope, const map<char, uint32_t>& local_scope, const string& evalme) {
  // Find {...} shit and replace it
  // {{...}} is registers
  // {|...|} is eval to hex number
  LOG(DEBUG) << "eval start: " << evalme;
  string out = "";
  int p = 0;
  int last = 0;
  while( (p = evalme.find_first_of("{", p)) != string::npos) {

    int close = find_matching(evalme, p, '{', '}');
    if(close == string::npos) {
      LOG(INFO) << "unmatched brace, failing";
      break;
    }
    string parsethis = evalme.substr(p, close-p+1);
    string replace;

    if(parsethis[1] == '{') {  // Register
      int register_num = stoi(EvalulateStringInScope(global_scope, local_scope, parsethis.substr(1, parsethis.length()-2)));
      if(register_num < 0 || register_num >= registers_.size()) {
        LOG(INFO) << "invalid register: " << register_num;
      } else {
        replace = registers_[register_num].first;
      }
    } else if(parsethis[1] == '|') {  // Hex
      string dataeval = EvalulateStringInScope(global_scope, local_scope, parsethis.substr(2, parsethis.length()-4) );
      LOG(DEBUG) << "  resolving: " << dataeval;
      replace = immed(memory_->ResolveToNumber(0, dataeval));
    } else {
      string variable = parsethis.substr(1, parsethis.length()-2);
      map<string, string>::const_iterator it = global_scope.find(variable);
      if(it != global_scope.end()) {
        replace = EvalulateStringInScope(global_scope, local_scope, it->second);  // Eval the globals
      } else {
        map<char, uint32_t>::const_iterator itl = local_scope.find(variable[0]);
        if(itl != local_scope.end()) {
          replace = immed(itl->second);
        } else {
          LOG(INFO) << "Variable " << variable << " not found in " << evalme;
          replace = "";
        }
      }
    }
    LOG(DEBUG) << "  parsing \"" << parsethis << "\" to \"" << replace << "\"";
    out += evalme.substr(last, p-last) + replace;

    p = close+1;
    last = p;
  }

  out += evalme.substr(last);
  LOG(DEBUG) << "eval done: \"" << evalme << "\" to \"" << out << "\"";
  return out;
}