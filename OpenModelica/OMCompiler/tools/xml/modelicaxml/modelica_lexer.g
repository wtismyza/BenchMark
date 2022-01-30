header {

}

options {
  language = "Cpp";
}

class modelica_lexer extends Lexer;

options {
    k=2;
    charVocabulary = '\3'..'\377';
    exportVocab = modelica;
    testLiterals = false;
    defaultErrorHandler = false;
}

tokens {
	ALGORITHM	= "algorithm"	;
	AND			= "and"	;
	ANNOTATION	= "annotation"	;
	BLOCK		= "block"	;
	CODE		= "Code"		;
	CLASS		= "class"	;
	CONNECT		= "connect"	;
	CONNECTOR	= "connector"	;
	CONSTANT	= "constant"	;
	DISCRETE	= "discrete"	;
    DER         = "der";
	EACH		= "each"	;
	ELSE		= "else"	;
	ELSEIF		= "elseif"	;
	ELSEWHEN	= "elsewhen"	;
  	END			= "end"		;
	ENUMERATION	= "enumeration"	;
	EQUATION	= "equation"	;
	ENCAPSULATED	= "encapsulated";
    EXPANDABLE  = "expandable";
	EXTENDS		= "extends"	;
	EXTERNAL	= "external"	;
	FALSE		= "false"	;
	FINAL		= "final"	;
	FLOW		= "flow"	;
	FOR		= "for"		;
	FUNCTION	= "function"	;
	IF		= "if"		;
	IMPORT		= "import"	;
	IN		= "in"		;
	INITIAL		= "initial"	;
	INNER		= "inner"	;
	INPUT		= "input"	;
	LOOP		= "loop"	;
	MODEL		= "model"	;
	NOT		= "not"		;
	OUTER		= "outer"	;
    OVERLOAD    = "overload";
	OR		= "or"		;
	OUTPUT		= "output"	;
	PACKAGE		= "package"	;
	PARAMETER	= "parameter"	;
	PARTIAL		= "partial"	;
	PROTECTED	= "protected"	;
	PUBLIC		= "public"	;
	RECORD		= "record"	;
	REDECLARE	= "redeclare"	;
	REPLACEABLE	= "replaceable"	;
	RESULTS		= "results"	;
	THEN		= "then"	;
	TRUE		= "true"	;
	TYPE		= "type"	;
	UNSIGNED_REAL	= "unsigned_real";
    DOT         = ".";
	WHEN		= "when"	;
	WHILE		= "while"	;
	WITHIN		= "within" 	;
	CONSTRAINEDBY = "constrainedby" ;
	RETURN		= "return"  ;
	BREAK		= "break"	;
	STREAM		= "stream"	; /* for Modelica 3.1 stream connectors */	
}


// ---------
// Operators
// ---------

LPAR		: '('	;
RPAR		: ')'	;
LBRACK		: '['	;
RBRACK		: ']'	;
LBRACE		: '{'	;
RBRACE		: '}'	;
EQUALS		: '='	;
ASSIGN		: ":="	;
PLUS		: '+'	;
MINUS		: '-'	;
STAR		: '*'	;
SLASH		: '/'	;
POWER		: '^'	;
/* element wise operators */
PLUS_EW     : ".+"  ;
MINUS_EW	: ".-"	;
STAR_EW		: ".*"	;
SLASH_EW	: "./"	;
POWER_EW	: ".^"	;

COMMA		: ','	;
LESS		: '<'	;
LESSEQ		: "<="	;
GREATER		: '>'	;
GREATEREQ	: ">="	;
EQEQ		: "=="	;
LESSGT		: "<>"	;
COLON		: ':'	;
SEMICOLON	: ';'	;

WS :
	(	' '
	|	'\t'
	|	( "\r\n" | '\r' |	'\n' ) { newline(); }
	)
	{ $setType(antlr::Token::SKIP); }
	;

ML_COMMENT :
		"/*"
		(options { generateAmbigWarnings=false; } : ML_COMMENT_CHAR
		| {LA(2)!='/'}? '*')*
		"*/" { $setType(antlr::Token::SKIP); } ;

protected
ML_COMMENT_CHAR :
		("\r\n" | '\n') { newline(); }
		| ~('*'|'\n'|'\r')
		;

SL_COMMENT :
		"//" (~('\n' | '\r') )*
		{  $setType(antlr::Token::SKIP); }
  	;

IDENT options { testLiterals = true; paraphrase = "an identifier";} :
		NONDIGIT (NONDIGIT | DIGIT)* | QIDENT
		;

protected
QIDENT options { testLiterals = true; paraphrase = "an identifier";} :
         '\'' (QCHAR | SESCAPE) (QCHAR | SESCAPE)* '\'' ;

protected
NONDIGIT : 	('_' | 'a'..'z' | 'A'..'Z');

protected
DIGIT :
	'0'..'9'
	;

protected
EXPONENT :
	('e'|'E') ('+' | '-')? (DIGIT)+
	;


UNSIGNED_INTEGER :
        (DIGIT)+ ('.' (DIGIT)* { $setType(UNSIGNED_REAL);} )?
        (EXPONENT { $setType(UNSIGNED_REAL); } )?
    |  ('.' (DIGIT)+ { $setType(UNSIGNED_REAL);}) (EXPONENT { $setType(UNSIGNED_REAL); } )?
    |  '.' { $setType(DOT); }
	;

STRING : '"'! (SCHAR | SESCAPE)* '"'!;


protected
SCHAR :	(options { generateAmbigWarnings=false; } : ('\n' | "\r\n"))	{ newline(); }
	| '\t'
	| ~('\n' | '\t' | '\r' | '\\' | '"');

protected
QCHAR :	(options { generateAmbigWarnings=false; } : ('\n' | "\r\n"))	{ newline(); }
	| '\t'
	| ~('\n' | '\t' | '\r' | '\\' | '\'');

protected
SESCAPE : '\\' ('\\' | '"' | "'" | '?' | 'a' | 'b' | 'f' | 'n' | 'r' | 't' | 'v');


protected
ESC :
	'\\'
	(	'"'
	|	'\\'
	)
	;





