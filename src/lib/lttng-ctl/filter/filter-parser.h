/* A Bison parser, made by GNU Bison 3.0.4.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

#ifndef YY_YY_FILTER_PARSER_H_INCLUDED
# define YY_YY_FILTER_PARSER_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 1
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    CHARACTER_CONSTANT_START = 258,
    SQUOTE = 259,
    STRING_LITERAL_START = 260,
    DQUOTE = 261,
    ESCSEQ = 262,
    CHAR_STRING_TOKEN = 263,
    DECIMAL_CONSTANT = 264,
    OCTAL_CONSTANT = 265,
    HEXADECIMAL_CONSTANT = 266,
    FLOAT_CONSTANT = 267,
    LSBRAC = 268,
    RSBRAC = 269,
    LPAREN = 270,
    RPAREN = 271,
    LBRAC = 272,
    RBRAC = 273,
    RARROW = 274,
    STAR = 275,
    PLUS = 276,
    MINUS = 277,
    MOD_OP = 278,
    DIV_OP = 279,
    RIGHT_OP = 280,
    LEFT_OP = 281,
    EQ_OP = 282,
    NE_OP = 283,
    LE_OP = 284,
    GE_OP = 285,
    LT_OP = 286,
    GT_OP = 287,
    AND_OP = 288,
    OR_OP = 289,
    NOT_OP = 290,
    ASSIGN = 291,
    COLON = 292,
    SEMICOLON = 293,
    DOTDOTDOT = 294,
    DOT = 295,
    EQUAL = 296,
    COMMA = 297,
    XOR_BIN = 298,
    AND_BIN = 299,
    OR_BIN = 300,
    NOT_BIN = 301,
    IDENTIFIER = 302,
    GLOBAL_IDENTIFIER = 303,
    ERROR = 304
  };
#endif
/* Tokens.  */
#define CHARACTER_CONSTANT_START 258
#define SQUOTE 259
#define STRING_LITERAL_START 260
#define DQUOTE 261
#define ESCSEQ 262
#define CHAR_STRING_TOKEN 263
#define DECIMAL_CONSTANT 264
#define OCTAL_CONSTANT 265
#define HEXADECIMAL_CONSTANT 266
#define FLOAT_CONSTANT 267
#define LSBRAC 268
#define RSBRAC 269
#define LPAREN 270
#define RPAREN 271
#define LBRAC 272
#define RBRAC 273
#define RARROW 274
#define STAR 275
#define PLUS 276
#define MINUS 277
#define MOD_OP 278
#define DIV_OP 279
#define RIGHT_OP 280
#define LEFT_OP 281
#define EQ_OP 282
#define NE_OP 283
#define LE_OP 284
#define GE_OP 285
#define LT_OP 286
#define GT_OP 287
#define AND_OP 288
#define OR_OP 289
#define NOT_OP 290
#define ASSIGN 291
#define COLON 292
#define SEMICOLON 293
#define DOTDOTDOT 294
#define DOT 295
#define EQUAL 296
#define COMMA 297
#define XOR_BIN 298
#define AND_BIN 299
#define OR_BIN 300
#define NOT_BIN 301
#define IDENTIFIER 302
#define GLOBAL_IDENTIFIER 303
#define ERROR 304

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED

union YYSTYPE
{
#line 329 "filter-parser.y" /* yacc.c:1909  */

	long long ll;
	char c;
	struct gc_string *gs;
	struct filter_node *n;

#line 159 "filter-parser.h" /* yacc.c:1909  */
};

typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif



int yyparse (struct filter_parser_ctx *parser_ctx, yyscan_t scanner);

#endif /* !YY_YY_FILTER_PARSER_H_INCLUDED  */
