#ifndef LEXER_TOKEN_H
#define LEXER_TOKEN_H
#pragma once
#include <string>
#include "punctuatortype.h"
#include "keywordtokentype.h"

#include "../utils/pos.h"

namespace Lexing {
  enum class TokenType {
    KEYWORD = 0,
    IDENTIFIER = 1,
    CONSTANT = 2,
    STRINGLITERAL = 3,
    PUNCTUATOR = 4,
    ILLEGAL = 5,
    END = 6,
  };

  enum class ConstantType {CHAR,INT,NULLPOINTER};

  class Token {
    public:
      Token(TokenType type, Pos posinfo, std::string value); 
      const TokenType & type() const {return this->m_type;}
      const Pos & pos() const {return this->m_posinfo;}
      std::string value() const {return this->m_value;}
    private:
      const TokenType m_type;
      const Pos m_posinfo; 
      const std::string m_value;
  };

  class PunctuatorToken : public Token {
    public:
      PunctuatorToken(TokenType type, Pos posinfo, std::string value);
      const PunctuatorType & punctype() const {return this->m_puncttype;};
    private:
      const PunctuatorType m_puncttype;
      static const PunctuatorType & string2punctuator(std::string value);
  };

  class KeywordToken : public Token {
    public:
      KeywordToken(TokenType type, Pos posinfo, std::string value);
      const KeywordType & keywordtype() const {return this->m_keywordtype;};
    private:
      const KeywordType & m_keywordtype;
      static const KeywordType & string2keyword(std::string value);
  };

  class ConstantToken : public Token {
    public:
      ConstantToken(Pos posinfo, std::string value, ConstantType type);
      const ConstantType type;
  };

/*
 *  operator== is NOT virtual; cast a Token to the subclass you want before
 *  using it
 */

inline bool operator==(const Token & lhs, const Token & rhs) {
  if (lhs.type() == rhs.type()) {
    return lhs.value() == rhs.value();
  }
  return false;
}

inline bool operator==(const PunctuatorToken & lhs, const PunctuatorToken & rhs) {
  return lhs.punctype() == rhs.punctype();
}

inline bool operator==(const PunctuatorToken & lhs, const PunctuatorType p) {
  return lhs.punctype() == p;
}

}

#endif
